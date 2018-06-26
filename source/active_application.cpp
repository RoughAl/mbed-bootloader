// ----------------------------------------------------------------------------
// Copyright 2016-2017 ARM Ltd.
//
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ----------------------------------------------------------------------------

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include "active_application.h"
#include "bootloader_common.h"
#include "unaligned_blockdevice.h"
#include "bd_sha256.h"

#include "update-client-common/arm_uc_metadata_header_v2.h"
#include "update-client-common/arm_uc_utilities.h"
#include "update-client-paal/arm_uc_paal_update.h"
#include "mbedtls/sha256.h"
#include "mbed.h"

#include <inttypes.h>

static FlashIAP flash;

bool activeStorageInit(void)
{
    int rc = flash.init();

    return (rc == 0);
}

void activeStorageDeinit(void)
{
    flash.deinit();
}

/**
 * Read the metadata header of the active image from internal flash
 * @param  headerP
 *             Caller-allocated header structure.
 * @return true if the read succeeds.
 */
bool readActiveFirmwareHeader(arm_uc_firmware_details_t* details)
{
    tr_debug("readActiveFirmwareHeader");

    bool result = false;

    if (details)
    {
        /* clear most recent UCP event */
        event_callback = CLEAR_EVENT;

        /* get active firmware details using UCP */
        arm_uc_error_t status = ARM_UCP_GetActiveFirmwareDetails(details);

        /* if the call was accepted,
           the event will indicate if the call succeeded
        */
        if (status.error == ERR_NONE)
        {
            /* wait until the event has been set */
            while (event_callback == CLEAR_EVENT)
            {
                __WFI();
            }

            /* mark the firmware details as valid if so indicated by the event */
            if (event_callback == ARM_UC_PAAL_EVENT_GET_ACTIVE_FIRMWARE_DETAILS_DONE)
            {
                result = true;
            }
        }
    }

    return result;
}

/**
 * Verify the integrity of the Active application
 * @detail Read the firmware in the ACTIVE app region and compute its hash.
 *         Compare the computed hash with the one given in the header
 *         to verify the ACTIVE firmware integrity
 * @param  headerP
 *             Caller-allocated header structure containing the hash and size
 *             of the firmware.
 * @return SUCCESS if the validation succeeds
 *         EMPTY   if no active application is present
 *         ERROR   if the validation fails
 */
int checkActiveApplication(arm_uc_firmware_details_t* details)
{
    tr_debug("checkActiveApplication");

    int result = RESULT_ERROR;

    if (details)
    {
        /* Read header and verify that it is valid */
        bool headerValid = readActiveFirmwareHeader(details);

        /* calculate hash if header is valid and slot is not empty */
        if ((headerValid) && (details->size > 0))
        {
            uint32_t appStart = MBED_CONF_APP_APPLICATION_START_ADDRESS;

            tr_debug("header start: 0x%08" PRIX32,
                     (uint32_t) FIRMWARE_METADATA_HEADER_ADDRESS);
            tr_debug("app start: 0x%08" PRIX32, appStart);
            tr_debug("app size: %" PRIu64, details->size);

            /* initialize hashing facility */
            mbedtls_sha256_context mbedtls_ctx;
            mbedtls_sha256_init(&mbedtls_ctx);
            mbedtls_sha256_starts(&mbedtls_ctx, 0);

            uint8_t SHA[SIZEOF_SHA256] = { 0 };
            uint32_t remaining = details->size;
            int32_t status = 0;

            /* read full image */
            while ((remaining > 0) && (status == 0))
            {
                /* read full buffer or what is remaining */
                uint32_t readSize = (remaining > BUFFER_SIZE) ?
                                    BUFFER_SIZE : remaining;

                /* read buffer using FlashIAP API for portability */
                status = flash.read(buffer_array,
                                    appStart + (details->size - remaining),
                                    readSize);

                /* update hash */
                mbedtls_sha256_update(&mbedtls_ctx, buffer_array, readSize);

                /* update remaining bytes */
                remaining -= readSize;

#if defined(SHOW_PROGRESS_BAR) && SHOW_PROGRESS_BAR == 1
                printProgress(details->size - remaining,
                              details->size);
#endif
            }

            /* finalize hash */
            mbedtls_sha256_finish(&mbedtls_ctx, SHA);
            mbedtls_sha256_free(&mbedtls_ctx);

            /* compare calculated hash with hash from header */
            int diff = memcmp(details->hash, SHA, SIZEOF_SHA256);

            if (diff == 0)
            {
                result = RESULT_SUCCESS;
            }
            else
            {
                printSHA256(details->hash);
                printSHA256(SHA);
            }
        }
        else if ((headerValid) && (details->size == 0))
        {
            /* header is valid but application size is 0 */
            result = RESULT_EMPTY;
        }
    }

    return result;
}

/**
 * Wipe the ACTIVE firmware region in the flash
 */
bool eraseActiveFirmware(uint32_t firmwareSize)
{
    tr_debug("eraseActiveFirmware");

    /* Find the exact end sector boundary. Some platforms have different sector
       sizes from sector to sector. Hence we count the sizes 1 sector at a time here */
    uint32_t erase_address = FIRMWARE_METADATA_HEADER_ADDRESS;
    uint32_t size_needed = FIRMWARE_METADATA_HEADER_SIZE + firmwareSize;
    while (erase_address < (FIRMWARE_METADATA_HEADER_ADDRESS + size_needed))
    {
        erase_address += flash.get_sector_size(erase_address);
    }

    /* check that the erase will not exceed MBED_CONF_APP_MAX_APPLICATION_SIZE */
    int result = -1;
    if (erase_address < (MBED_CONF_APP_MAX_APPLICATION_SIZE + \
                         MBED_CONF_APP_APPLICATION_START_ADDRESS))
    {
        tr_debug("Erasing from 0x%08" PRIX32 " to 0x%08" PRIX32,
                 (uint32_t) FIRMWARE_METADATA_HEADER_ADDRESS,
                 (uint32_t) erase_address);

        /* Erase flash to make place for new application. Erasing sector by sector as some
           platforms have varible sector sizes and mbed-os cannot deal with erasing multiple
           sectors successfully in that case. https://github.com/ARMmbed/mbed-os/issues/6077 */
        erase_address = FIRMWARE_METADATA_HEADER_ADDRESS;
        while (erase_address < (FIRMWARE_METADATA_HEADER_ADDRESS + size_needed))
        {
            uint32_t sector_size = flash.get_sector_size(erase_address);
            result = flash.erase(erase_address,
                                 sector_size);
            if (result != 0)
            {
                tr_debug("Erasing from 0x%08" PRIX32 " to 0x%08" PRIX32 " failed with retval %i",
                         erase_address, erase_address + sector_size, result);
                break;
            }
            else
            {
                erase_address += sector_size;
            }
        }
    }
    else
    {
        tr_error("Firmware size 0x%" PRIX32 " rounded up to the nearest sector boundary 0x%" \
                 PRIX32 " is larger than the maximum application size 0x%" PRIX32,
                 firmwareSize, erase_address - MBED_CONF_APP_APPLICATION_START_ADDRESS,
                 MBED_CONF_APP_MAX_APPLICATION_SIZE);
    }

    return (result == 0);
}

bool writeActiveFirmwareHeader(arm_uc_firmware_details_t* details)
{
    printf("writeActiveFirmwareHeader\n");

    tr_debug("writeActiveFirmwareHeader");

    bool result = false;

    if (details)
    {
        /* round up program size to nearest page size */
        const uint32_t pageSize = flash.get_page_size();
        const uint32_t programSize = (ARM_UC_INTERNAL_HEADER_SIZE_V2 + pageSize - 1)
                                     / pageSize * pageSize;

        /* coverity[no_escape] */
        MBED_BOOTLOADER_ASSERT((programSize <= BUFFER_SIZE),
               "Header program size %" PRIu32 " bigger than buffer %d\r\n",
               programSize, BUFFER_SIZE);

        /* coverity[no_escape] */
        MBED_BOOTLOADER_ASSERT((programSize <= FIRMWARE_METADATA_HEADER_SIZE),
               "Header program size %" PRIu32 " bigger than expected header %d\r\n",
               programSize, FIRMWARE_METADATA_HEADER_SIZE);

        /* pad buffer to 0xFF */
        memset(buffer_array, 0xFF, programSize);

        /* create internal header in temporary buffer */
        arm_uc_buffer_t output_buffer = {
            .size_max = BUFFER_SIZE,
            .size     = 0,
            .ptr      = buffer_array
        };

        arm_uc_error_t status = arm_uc_create_internal_header_v2(details,
                                                                 &output_buffer);

        if ((status.error == ERR_NONE) &&
            (output_buffer.size == ARM_UC_INTERNAL_HEADER_SIZE_V2))
        {
            /* write header using FlashIAP API */
            int ret = flash.program(buffer_array,
                                    FIRMWARE_METADATA_HEADER_ADDRESS,
                                    programSize);

            result = (ret == 0);
        }
    }

    return result;
}

bool writeActiveFirmware(uint32_t index, arm_uc_firmware_details_t* details)
{
    tr_debug("writeActiveFirmware");

    bool result = false;

    if (details)
    {
        const uint32_t pageSize = flash.get_page_size();

        /* we require app_start_addr fall on a page size boundary */
        uint32_t app_start_addr = MBED_CONF_APP_APPLICATION_START_ADDRESS;

        /* coverity[no_escape] */
        MBED_BOOTLOADER_ASSERT((app_start_addr % pageSize) == 0,
               "Application (0x%" PRIX32 ") does not start on a "
               "page size (0x%" PRIX32 ") aligned address\r\n",
               app_start_addr,
               pageSize);

        /* round down the read size to a multiple of the page size
           that still fits inside the main buffer.
        */
        uint32_t readSize = (BUFFER_SIZE / pageSize) * pageSize;

        arm_uc_buffer_t buffer = {
            .size_max = readSize,
            .size     = 0,
            .ptr      = buffer_array
        };

        int retval = 0;
        uint32_t offset = 0;

        /* write firmware */
        while ((offset < details->size) &&
               (retval == 0))
        {
            /* clear most recent UCP event */
            event_callback = CLEAR_EVENT;

            /* set the number of bytes expected */
            buffer.size = (details->size - offset) > buffer.size_max ?
                            buffer.size_max : (details->size - offset);

            /* fill buffer using UCP */
            arm_uc_error_t ucp_status = ARM_UCP_Read(index, offset, &buffer);

            /* wait for event if the call is accepted */
            if (ucp_status.error == ERR_NONE)
            {
                while (event_callback == CLEAR_EVENT)
                {
                    __WFI();
                }
            }

            /* check status and actual read size */
            if ((event_callback == ARM_UC_PAAL_EVENT_READ_DONE) &&
                (buffer.size > 0))
            {
                /* the last page, in the last buffer might not be completely
                   filled, round up the program size to include the last page
                */
                uint32_t programOffset = 0;
                uint32_t programSize = (buffer.size + pageSize - 1)
                                       / pageSize * pageSize;

                /* write one page at a time */
                while ((programOffset < programSize) &&
                       (retval == 0))
                {
                    retval = flash.program(&(buffer.ptr[programOffset]),
                                           app_start_addr + offset + programOffset,
                                           pageSize);

                    programOffset += pageSize;

#if defined(SHOW_PROGRESS_BAR) && SHOW_PROGRESS_BAR == 1
                    printProgress(offset + programOffset, details->size);
#endif
                }

                tr_debug("\r\n%" PRIu32 "/%" PRIu32 " writing %" PRIu32 " bytes to 0x%08" PRIX32,
                         offset, (uint32_t) details->size, programSize, app_start_addr + offset);

                offset += programSize;
            }
            else
            {
                tr_error("ARM_UCP_Read returned 0 bytes");

                /* set error and break out of loop */
                retval = -1;
                break;
            }
        }

        result = (retval == 0);
    }

    return result;
}

/*
 * Copy loop to update the application
 */
bool copyStoredApplication(uint32_t index,
                           arm_uc_firmware_details_t* details)
{
    tr_debug("copyStoredApplication");

    bool result = false;

    /*************************************************************************/
    /* Step 1. Erase active application                                      */
    /*************************************************************************/

    result = eraseActiveFirmware(details->size);

    /*************************************************************************/
    /* Step 2. Write header                                                  */
    /*************************************************************************/

    if (result)
    {
        result = writeActiveFirmwareHeader(details);
    }

    /*************************************************************************/
    /* Step 3. Copy application                                              */
    /*************************************************************************/

    if (result)
    {
        result = writeActiveFirmware(index, details);
    }

    /*************************************************************************/
    /* Step 4. Verify application                                            */
    /*************************************************************************/

    if (result)
    {
        tr_info("Verify new active firmware:");

        int recheck = checkActiveApplication(details);

        result = (recheck == RESULT_SUCCESS);
    }

    return result;
}

/**
 * Copy the current firmware into external flash
 * @param details Information about the active firmware
 * @param bdOffset Offset in external flash,
 *                 need to have enough space for the full application plus the size of the arm_uc_firmware_details_t struct.
 *                 Does not need to be aligned.
 *
 *
 * @return SUCCESS if the copy succeeds
 *         EMPTY   if no active application is present
 *         ERROR   if the copy fails
 */
int copyActiveApplicationIntoFlash(BlockDevice* bd, uint32_t bdOffset)
{
    printf("Copying active firmware into external flash...\n");

    UnalignedBlockDevice ubd(bd);
    int bd_status = ubd.init();
    if (bd_status != BD_ERROR_OK) {
        tr_warn("Could not initialize unaligned block device (%d)", bd_status);
        return RESULT_ERROR;
    }

    int result = RESULT_ERROR;

    /* read current active firmware details from flash */
    arm_uc_firmware_details_t details;

    /* Read header and verify that it is valid */
    bool headerValid = readActiveFirmwareHeader(&details);

    /* calculate hash if header is valid and slot is not empty */
    if ((headerValid) && (details.size > 0))
    {
        /* Look at what's currently in flash, and if it's already correct */
        arm_uc_firmware_details_t curr_details;
        bd_status = ubd.read(&curr_details, bdOffset, sizeof(arm_uc_firmware_details_t));
        if (bd_status != BD_ERROR_OK) {
            /* This is a sign we cannot access the block device, so don't continue */
            tr_warn("Could not read current details\n");
            return RESULT_ERROR;
        }

        tr_debug("Details:\n");
        tr_debug("New size=%llu version=%llu\n", details.size, details.version);
        tr_debug("Old size=%llu version=%llu\n", curr_details.size, curr_details.version);

        if (curr_details.version == details.version && curr_details.size == details.size) {
            tr_info("Version and size match, right firmware already in place: abort copy\n");
            return RESULT_SUCCESS;
        }

        tr_info("Version or size mismatch, copying firmware...\n");

        /* Copy the details structure into flash */
        size_t offset = bdOffset + sizeof(arm_uc_firmware_details_t);

        uint32_t appStart = MBED_CONF_APP_APPLICATION_START_ADDRESS;

        uint32_t remaining = details.size;
        int32_t status = 0;

        /* read full image */
        while ((remaining > 0) && (status == 0))
        {
            /* read full buffer or what is remaining */
            uint32_t readSize = (remaining > BUFFER_SIZE) ?
                                BUFFER_SIZE : remaining;

            /* read buffer using FlashIAP API for portability */
            status = flash.read(buffer_array,
                                appStart + (details.size - remaining),
                                readSize);

            /* and write it to external flash */
            ubd.program(buffer_array, offset, readSize);

            /* flash driver does not like writing quickly? */
            wait_ms(100);

            /* update remaining bytes */
            remaining -= readSize;
            offset += readSize;

#if defined(SHOW_PROGRESS_BAR) && SHOW_PROGRESS_BAR == 1
            printProgress(details.size - remaining,
                            details.size);
#endif
        }

        unsigned char shaInBd[SIZEOF_SHA256];

        BdSha256 bdSha256(&ubd, buffer_array, BUFFER_SIZE);
        bdSha256.calculate(bdOffset + sizeof(arm_uc_firmware_details_t), details.size, shaInBd);

        /* compare calculated hash with hash from header */
        int diff = memcmp(details.hash, shaInBd, SIZEOF_SHA256);

        if (diff == 0)
        {
            printSHA256(details.hash);
            result = RESULT_SUCCESS;
        }
        else
        {
            printSHA256(details.hash);
            printSHA256(shaInBd);

            return RESULT_ERROR;
        }

        /* copy the new details structure */
        ubd.program(&details, bdOffset, sizeof(arm_uc_firmware_details_t));

        return RESULT_SUCCESS;
    }
    else if ((headerValid) && (details.size == 0))
    {
        /* header is valid but application size is 0 */
        result = RESULT_EMPTY;
    }

    return result;
}
