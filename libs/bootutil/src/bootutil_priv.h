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

#ifndef H_BOOTUTIL_PRIV_
#define H_BOOTUTIL_PRIV_

#include "bootutil/image.h"

#define BOOT_EFLASH     1
#define BOOT_EFILE      2
#define BOOT_EBADIMAGE  3
#define BOOT_EBADVECT   4
#define BOOT_EBADSTATUS 5
#define BOOT_ENOMEM     6

#define BOOT_TMPBUF_SZ  256

struct boot_image_location {
    uint8_t bil_flash_id;
    uint32_t bil_address;
};

/*
 * Maintain state of copy progress.
 */
struct boot_status {
    uint32_t idx;
    uint32_t state;
};

/*
 * End-of-image data structure.
 */
#define BOOT_IMG_MAGIC  0x12344321
struct boot_img_trailer {
    uint32_t bit_start;
    uint32_t bit_done;
};

int bootutil_verify_sig(uint8_t *hash, uint32_t hlen, uint8_t *sig, int slen,
    uint8_t key_id);

int boot_read_image_header(struct boot_image_location *loc,
  struct image_header *out_hdr);
int boot_write_status(struct boot_status *bs);
int boot_read_status(struct boot_status *bs);
void boot_clear_status(void);

#endif

