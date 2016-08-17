/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <assert.h>
#include <stddef.h>
#include <inttypes.h>
#include <string.h>
#include <hal/flash_map.h>
#include <hal/hal_flash.h>
#include <os/os_malloc.h>
#include <console/console.h>
#include "bootutil/loader.h"
#include "bootutil/image.h"
#include "bootutil/bootutil_misc.h"
#include "bootutil_priv.h"

/** Number of image slots in flash; currently limited to two. */
#define BOOT_NUM_SLOTS              2

/** The request object provided by the client. */
static const struct boot_req *boot_req;

/** Image headers read from flash. */
static struct boot_img {
    struct image_header hdr;
    struct boot_image_location loc;
    uint32_t area;
} boot_img[BOOT_NUM_SLOTS];

static struct boot_status boot_state;

static int boot_erase_area(int area_idx, uint32_t sz);
static uint32_t boot_copy_sz(int max_idx, int *cnt);

/**
 * Calculates the flash offset of the specified image slot.
 *
 * @param slot_num              The number of the slot to calculate.
 *
 * @return                      The flash offset of the image slot.
 */
static void
boot_slot_addr(int slot_num, struct boot_image_location *loc)
{
    const struct flash_area *area_desc;
    uint8_t area_idx;

    area_idx = boot_req->br_slot_areas[slot_num];
    area_desc = boot_req->br_area_descs + area_idx;
    loc->bil_flash_id = area_desc->fa_flash_id;
    loc->bil_address = area_desc->fa_off;
}

static uint32_t
boot_magic_off(int slot_num)
{
    return boot_img[slot_num].area + boot_img[slot_num].loc.bil_address -
      sizeof(struct boot_img_trailer);
}

static uint32_t
boot_scratch_off(void)
{
    struct flash_area *scratch;
    uint32_t off;
    int cnt;

    scratch = &boot_req->br_area_descs[boot_req->br_scratch_area_idx];
    off = boot_copy_sz(boot_req->br_slot_areas[1], &cnt);
    off += (scratch->fa_off - sizeof(struct boot_img_trailer));
    return off;
}

static void
boot_slot_magic(int slot_num, struct boot_img_trailer *bit)
{
    uint32_t off;
    struct boot_img *b;

    b = &boot_img[slot_num];
    off = boot_magic_off(slot_num);
    memset(bit, 0xff, sizeof(*bit));
    hal_flash_read(b->loc.bil_flash_id, off, bit, sizeof(*bit));
}

static void
boot_scratch_magic(struct boot_img_trailer *bit)
{
    uint32_t off;
    struct flash_area *scratch;

    scratch = &boot_req->br_area_descs[boot_req->br_scratch_area_idx];

    off = boot_scratch_off();
    hal_flash_read(scratch->fa_flash_id, off, bit, sizeof(*bit));
}

void
boot_image_info(void)
{
    int i;
    struct boot_img *b;

    for (i = 0; i < BOOT_NUM_SLOTS; i++) {
        b = &boot_img[i];
        boot_slot_addr(i, &b->loc);
        boot_read_image_header(&b->loc, &b->hdr);
        b->area = boot_req->br_img_sz;
    }
}

/*
 * Validate image hash/signature in a slot.
 */
static int
boot_image_check(struct image_header *hdr, struct boot_image_location *loc)
{
    static void *tmpbuf;

    if (!tmpbuf) {
        tmpbuf = malloc(BOOT_TMPBUF_SZ);
        if (!tmpbuf) {
            return BOOT_ENOMEM;
        }
    }
    if (bootutil_img_validate(hdr, loc->bil_flash_id, loc->bil_address,
        tmpbuf, BOOT_TMPBUF_SZ)) {
        return BOOT_EBADIMAGE;
    }
    return 0;
}

/**
 * Selects a slot number to boot from.
 *
 * @return                      The slot number to boot from on success;
 *                              -1 if an appropriate slot could not be
 *                              determined.
 */
static int
boot_select_image_slot(void)
{
    /*
     * Check for swap magic. Check the integrity of the suggested image.
     */
    int rc;
    int i;
    struct boot_img *b;
    struct boot_img_trailer bit;

    for (i = 1; i < BOOT_NUM_SLOTS; i++) {
        b = &boot_img[i];
        boot_slot_magic(i, &bit);
        if (bit.bit_start == BOOT_IMG_MAGIC) {
            rc = boot_image_check(&b->hdr, &b->loc);
            if (rc) {
                /*
                 * Image fails integrity check. Erase it.
                 */
                boot_erase_area(boot_req->br_slot_areas[i], b->area);
            } else {
                return i;
            }
        }
    }
    return 0;
}

static int
boot_status_sz(void)
{
    return sizeof(struct boot_img_trailer) + 32 * sizeof(uint32_t);
}

/*
 * How many sectors starting from sector[idx] can fit inside scratch.
 *
 */
static uint32_t
boot_copy_sz(int max_idx, int *cnt)
{
    int i;
    uint32_t sz;
    static uint32_t scratch_sz = 0;

    if (!scratch_sz) {
        for (i = boot_req->br_scratch_area_idx;
             i < boot_req->br_num_image_areas;
             i++) {
            scratch_sz += boot_req->br_area_descs[i].fa_size;
        }
    }
    sz = 0;
    *cnt = 0;
    for (i = max_idx - 1; i >= 0; i--) {
        if (sz + boot_req->br_area_descs[i].fa_size > scratch_sz) {
            break;
        }
        sz += boot_req->br_area_descs[i].fa_size;
        *cnt = *cnt + 1;
    }
    return sz;
}

static int
boot_erase_area(int area_idx, uint32_t sz)
{
    const struct flash_area *area_desc;
    int rc;

    area_desc = boot_req->br_area_descs + area_idx;
    rc = hal_flash_erase(area_desc->fa_flash_id, area_desc->fa_off, sz);
    if (rc != 0) {
        return BOOT_EFLASH;
    }
    return 0;
}

/**
 * Copies the contents of one area to another.  The destination area must
 * be erased prior to this function being called.
 *
 * @param from_area_idx       The index of the source area.
 * @param to_area_idx         The index of the destination area.
 * @param sz                  The number of bytes to move.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
boot_copy_area(int from_area_idx, int to_area_idx, uint32_t sz)
{
    const struct flash_area *from_area_desc;
    const struct flash_area *to_area_desc;
    uint32_t from_addr;
    uint32_t to_addr;
    uint32_t off;
    int chunk_sz;
    int rc;

    static uint8_t buf[1024];

    from_area_desc = boot_req->br_area_descs + from_area_idx;
    to_area_desc = boot_req->br_area_descs + to_area_idx;

    assert(to_area_desc->fa_size >= from_area_desc->fa_size);

    off = 0;
    while (off < sz) {
        if (sz - off > sizeof buf) {
            chunk_sz = sizeof buf;
        } else {
            chunk_sz = sz - off;
        }

        from_addr = from_area_desc->fa_off + off;
        rc = hal_flash_read(from_area_desc->fa_flash_id, from_addr, buf,
                            chunk_sz);
        if (rc != 0) {
            return rc;
        }

        to_addr = to_area_desc->fa_off + off;
        rc = hal_flash_write(to_area_desc->fa_flash_id, to_addr, buf,
                             chunk_sz);
        if (rc != 0) {
            return rc;
        }

        off += chunk_sz;
    }

    return 0;
}

/**
 * Swaps the contents of two flash areas.
 *
 * @param area_idx_1          The index of one area to swap.  This area
 *                                  must be part of the first image slot.
 * @param area_idx_2          The index of the other area to swap.  This
 *                                  area must be part of the second image
 *                                  slot.
 * @return                      0 on success; nonzero on failure.
 */
static int
boot_swap_areas(int idx, uint32_t sz, int end_area)
{
    int area_idx_1;
    int area_idx_2;
    int rc;

    area_idx_1 = boot_req->br_slot_areas[0] + idx;
    area_idx_2 = boot_req->br_slot_areas[1] + idx;
    assert(area_idx_1 != area_idx_2);
    assert(area_idx_1 != boot_req->br_scratch_area_idx);
    assert(area_idx_2 != boot_req->br_scratch_area_idx);

    if (boot_state.state == 0) {
        rc = boot_erase_area(boot_req->br_scratch_area_idx, sz);
        if (rc != 0) {
            return rc;
        }

        rc = boot_copy_area(area_idx_2, boot_req->br_scratch_area_idx, sz);
        if (rc != 0) {
            return rc;
        }

        boot_state.state = 1;
        (void)boot_write_status(&boot_state);
    }
    if (boot_state.state == 1) {
        rc = boot_erase_area(area_idx_2, sz);
        if (rc != 0) {
            return rc;
        }

        rc = boot_copy_area(area_idx_1, area_idx_2,
          end_area ? (sz - boot_status_sz()) : sz);
        if (rc != 0) {
            return rc;
        }

        boot_state.state = 2;
        (void)boot_write_status(&boot_state);
    }
    if (boot_state.state == 2) {
        rc = boot_erase_area(area_idx_1, sz);
        if (rc != 0) {
            return rc;
        }

        rc = boot_copy_area(boot_req->br_scratch_area_idx, area_idx_1, sz);
        if (rc != 0) {
            return rc;
        }

        boot_state.idx++;
        boot_state.state = 0;
        (void)boot_write_status(&boot_state);
    }
    return 0;
}

/**
 * Swaps the two images in flash.  If a prior copy operation was interrupted
 * by a system reset, this function completes that operation.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
boot_copy_image(void)
{
    uint32_t sz;
    int i;
    int end_area = 1;
    int cnt;
    int cur_idx;

    for (i = boot_req->br_slot_areas[1], cur_idx = 0; i > 0; cur_idx++) {
        sz = boot_copy_sz(i, &cnt);
        i -= cnt;
        if (cur_idx >= boot_state.idx) {
            boot_swap_areas(i, sz, end_area);
        }
        end_area = 0;
    }
    boot_clear_status();

    return 0;
}


/*
 * Is copy in progress?
 */
static void
boot_read_status_bytes(struct boot_status *bs, uint8_t flash_id, uint32_t off)
{
    uint8_t status;

    off -= sizeof(status) * 2;
    while (1) {
        hal_flash_read(flash_id, off, &status, sizeof(status));
        if (status == 0xff) {
            break;
        }
        off--;
        if (bs->state == 2) {
            bs->idx++;
            bs->state = 0;
        } else {
            bs->state++;
        }
    }
}

int
boot_read_status(struct boot_status *bs)
{
    struct boot_img_trailer bit;
    struct flash_area *scratch;

    /*
     * Check if boot_img_trailer is in scratch, or at the end of slot0.
     */
    boot_slot_magic(0, &bit);
    if (bit.bit_start == BOOT_IMG_MAGIC && bit.bit_done == 0xffffffff) {
        boot_read_status_bytes(bs, boot_img[0].loc.bil_flash_id,
          boot_magic_off(0));
        console_printf("status in slot0, %lu/%lu\n", bs->idx, bs->state);
        return 1;
    }
    boot_scratch_magic(&bit);
    if (bit.bit_start == BOOT_IMG_MAGIC && bit.bit_done == 0xffffffff) {
        scratch = &boot_req->br_area_descs[boot_req->br_scratch_area_idx];
        boot_read_status_bytes(bs, scratch->fa_flash_id, boot_scratch_off());
        console_printf("status in scratch, %lu/%lu\n", bs->idx, bs->state);
        return 1;
    }
    return 0;
}

#include <hal/hal_system.h>

int
boot_write_status(struct boot_status *bs)
{
    uint32_t off;
    uint8_t flash_id;
    uint8_t val;

    if (bs->idx == 0) {
        /*
         * Write to scratch
         */
        off = boot_scratch_off();
        flash_id =
          boot_req->br_area_descs[boot_req->br_scratch_area_idx].fa_flash_id;
    } else {
        /*
         * Write to slot 0;
         */
        off = boot_magic_off(0);
        flash_id = boot_img[0].loc.bil_flash_id;
    }
    off -= ((3 * sizeof(uint8_t)) * bs->idx +
      sizeof(uint8_t) * (bs->state + 1));

    console_printf("status write, %lu/%lu -> %lx\n", bs->idx, bs->state, off);

    val = bs->state;
    hal_flash_write(flash_id, off, &val, sizeof(val));

    return 0;
}

void
boot_clear_status(void)
{
    uint32_t off;
    uint32_t val = BOOT_IMG_MAGIC;
    uint8_t flash_id;

    /*
     * Write to slot 0;
     */
    off = boot_magic_off(0);
    flash_id = boot_img[0].loc.bil_flash_id;
    off += sizeof(uint32_t);
    hal_flash_write(flash_id, off, &val, sizeof(val));
}

/**
 * Prepares the booting process.  Based on the information provided in the
 * request object, this function moves images around in flash as appropriate,
 * and tells you what address to boot from.
 *
 * @param req                   Contains information about the flash layout.
 * @param rsp                   On success, indicates how booting should occur.
 *
 * @return                      0 on success; nonzero on failure.
 */
int
boot_go(const struct boot_req *req, struct boot_rsp *rsp)
{
    int slot;
    int rc;

    /* Set the global boot request object.  The remainder of the boot process
     * will reference the global.
     */
    boot_req = req;

    /* Attempt to read an image header from each slot. */
    boot_image_info();

    /* Read the boot status to determine if an image copy operation was
     * interrupted (i.e., the system was reset before the boot loader could
     * finish its task last time).
     */
    if (boot_read_status(&boot_state)) {
        /* We are resuming an interrupted image copy. */
        rc = boot_copy_image();
        if (rc != 0) {
            /* We failed to put the images back together; there is really no
             * solution here.
             */
            return rc;
        }
    }

    /*
     * Check if we should initiate copy.
     */
    slot = boot_select_image_slot();
    if (slot == -1) {
        return BOOT_EBADIMAGE;
    }

    if (slot) {
        rc = boot_copy_image();
        if (rc) {
            return rc;
        }
    }

    /* Always boot from the primary slot. */
    rsp->br_flash_id = boot_img[0].loc.bil_flash_id;
    rsp->br_image_addr = boot_img[0].loc.bil_address;
    rsp->br_hdr = &boot_img[0].hdr;

    return 0;
}
