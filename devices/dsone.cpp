/*
Original source code
---------------------------------------------------
DSTT flashcart_core implementation by HandsomeMatt
---------------------------------------------------



for DSONE SDHC flashcart_core by multi-vitamin


Flash chips are all identified by the following command sequence:
    0x555:0xAA, 0x2AA:0x55, 0x555:0x90

Supported Chips using the same standard of command definitons (type A):
    Sector Block Addressing = 16KB,8KB,8KB,32KB,64KB,64KB,64KB... (unless otherwise specified)

    0x7DBF: https://ww1.microchip.com/downloads/aemDocuments/documents/MPD/ProductDocuments/DataSheets/SST39LFX00A-SST39VFX00A-2-Mbit-4-Mbit-8-Mbit-x16-Multi-Purpose-Flash-DS20005001.pdf (4K sector-erase)


Supported but untested, non standard flash commands:


Known flashchips that are "unsupported":

*/

#include "../device.h"

#include <stdlib.h>
#include <cstring>

namespace flashcart_core {
using platform::logMessage;
using platform::showProgress;

const uint16_t supported_flashchips[] = {
    0xD7BF
};

// Header: TOP TF/SD DSONEDS
// Device ID: 0xFC2
// Sector Size: 0x2000
class DSONE : Flashcart {
private:
    uint32_t m_flashchip;

    enum {
        DSONE_CMD_TYPE_1,
        DSONE_CMD_TYPE_2
    } m_cmd_type;

    uint32_t DSONE_flash_command(uint8_t data0, uint32_t data1, uint16_t data2)
    {
        uint8_t cmd[8];
        cmd[0] = data0;
        cmd[1] = (uint8_t)((data1 >> 24)&0xFF);
        cmd[2] = (uint8_t)((data1 >> 16)&0xFF);
        cmd[3] = (uint8_t)((data1 >>  8)&0xFF);
        cmd[4] = (uint8_t)((data1 >>  0)&0xFF);
        cmd[5] = (uint8_t)((data2 >>  8)&0xFF);
        cmd[6] = (uint8_t)((data2 >>  0)&0xFF);
        cmd[7] = 0x00;

        uint32_t ret;

        m_card->sendCommand(cmd, (uint8_t*)&ret, 4, 0xa7180000);
        return ret;
    }

    void DSONE_reset()
    {
        logMessage(LOG_DEBUG, "DSONE: Reset");
        if (m_cmd_type == DSONE_CMD_TYPE_2) {
            //DSONE_flash_command(0x87, 0, 0xFF);
        } else if (m_cmd_type == DSONE_CMD_TYPE_1) {
            DSONE_flash_command(0x87, 0, 0xF0);
        }
    }

    uint32_t get_flashchip_id()
    {
        uint32_t flashchip;

        DSONE_flash_command(0x87, 0x5555, 0xAA);
        DSONE_flash_command(0x87, 0x2AAA, 0x55);
        DSONE_flash_command(0x87, 0x5555, 0x90);
        flashchip = DSONE_flash_command(0, 0, 0);
        DSONE_reset();
		/*
        if ((flashchip & 0xFF00FFFF) != 0x7F003437 && (flashchip & 0xFF00FFFF) != 0x7F00B537
              && (uint16_t)flashchip != 0x41F && (uint16_t)flashchip != 0x51F)
        {
            DSONE_flash_command(0x87, 0x5555, 0xAA);
            DSONE_flash_command(0x87, 0x2AAA, 0x55);
            DSONE_flash_command(0x87, 0x5555, 0x90);
            uint32_t device_id = DSONE_flash_command(0, 0x100, 0);
            DSONE_reset();

            if ((uint16_t)device_id == 0xBA1C || (uint16_t)device_id == 0xB91C)
                return device_id;
        }
		*/

        return flashchip;
    }

    bool flashchip_supported(uint32_t flashchip)
    {
		/*
		// there's probably a better way to do this?
		if ((uint16_t)flashchip == 0xed01) {
			// AMD AM29LV001BT
			// check for sector write protection, if it's enabled we can't do much
			uint8_t writeProtected = false;
			uint32_t address = 0;
			for (; address < 0x10000; address += 0x4000) {
				DSONE_flash_command(0x87, 0x5555, 0xAA);
				DSONE_flash_command(0x87, 0x2AAA, 0x55);
				DSONE_flash_command(0x87, 0x5555, 0x90);
				writeProtected = (uint8_t)(DSONE_flash_command(0, address | 2, 0));
				DSONE_reset();
				if (writeProtected) break;
			};
			if (writeProtected != 0) {
				logMessage(LOG_NOTICE, "DSONE: Flashchip supported, but sector %d is write protected", address >> 14);
			}
			return (writeProtected == 0);
		}
		*/
		
        for (unsigned int i = 0; i < sizeof(supported_flashchips) / 2; ++i)
            if (supported_flashchips[i] == (uint16_t)flashchip)
                return true;

        return false;
    }

    void Erase_Block(uint32_t offset, uint32_t length)
    {
        logMessage(LOG_DEBUG, "DSONE: erase_block(0x%08x)", offset);
        if (m_cmd_type == DSONE_CMD_TYPE_1) {
            DSONE_flash_command(0x87, 0x5555, 0xAA);
            DSONE_flash_command(0x87, 0x2AAA, 0x55);
            DSONE_flash_command(0x87, 0x5555, 0x80);
            DSONE_flash_command(0x87, 0x5555, 0xAA);
            DSONE_flash_command(0x87, 0x2AAA, 0x55);

            DSONE_flash_command(0x87, offset, 0x30);
        } else if (m_cmd_type == DSONE_CMD_TYPE_2) {
			/*
            DSONE_flash_command(0x87, 0x00,   0x50); // Clear Status Register
            DSONE_flash_command(0x87, offset, 0x20); // Erase Setup
            DSONE_flash_command(0x87, offset, 0xD0); // Erase Confirm

            // TODO: Timeout if something goes wrong.
            while (!(DSONE_flash_command(0, offset & 0xFFFFFFFC, 0) & 0x80));

            DSONE_flash_command(0x87, 0x00, 0x50); // Clear Status Register
            DSONE_flash_command(0x87, 0x00, 0xFF); // Reset
			*/
        }

        uint32_t end_offset = offset + length;
        for (; offset < end_offset; offset += 4)
        {
            // TODO: Timeout if something goes wrong.
            while (DSONE_flash_command(0, offset, 0) != 0xFFFFFFFF);
        }
    }

    void Erase_Chip(uint32_t offset) {
        std::vector<uint32_t> erase_blocks;
        logMessage(LOG_INFO, "DSONE: Erasing Flash");

        switch(m_flashchip)
        {
			/*
            case 0xD7BF:
				erase_blocks = std::vector<uint32_t>(0x10, 0x1000);
				break;
			*/

            default:
                erase_blocks = std::vector<uint32_t>(0x10, 0x1000);
                break;
        }

        // calculate the max so we can show progress
        uint32_t erase_endaddr = offset;
        for (auto const& block_sz: erase_blocks) {
            erase_endaddr += block_sz;
        }

        uint32_t erase_addr = offset;
        for (auto const& block_sz: erase_blocks) {
            showProgress(erase_addr, erase_endaddr, "Erasing Blocks");
            Erase_Block(erase_addr, block_sz);
            erase_addr += block_sz;
        }
    }

    // pretty messy function, but gets the job done
    void Program_Byte(uint32_t offset, uint8_t data)
    {
        logMessage(LOG_DEBUG, "DSONE: program_byte(0x%08x) = 0x%02x", offset, data);
        if (m_cmd_type == DSONE_CMD_TYPE_2) {
			/*
            DSONE_flash_command(0x87, 0x00,   0x50); // Clear Status Register
            DSONE_flash_command(0x87, offset, 0x40); // Word Write
            DSONE_flash_command(0x87, offset, data);

            // TODO: Timeout if something goes wrong.
            while (!(DSONE_flash_command(0, offset & 0xFFFFFFFC, 0) & 0x80));

            DSONE_flash_command(0x87, 0x00, 0x50); // Clear Status Register
            //DSONE_flash_command(0x87, offset, 0xFF); // Reset (offset not required)
			*/
        } else if (m_cmd_type == DSONE_CMD_TYPE_1) {
            DSONE_flash_command(0x87, 0x5555, 0xAA);
            DSONE_flash_command(0x87, 0x2AAA, 0x55);
            DSONE_flash_command(0x87, 0x5555, 0xA0);
            DSONE_flash_command(0x87, offset, data);

            // TODO: Timeout if something goes wrong.
            while ((uint8_t)DSONE_flash_command(0, offset, 0) != data);
        }
    }

public:
    DSONE() : Flashcart("DSONE", 0x80000) { }

    const char *getAuthor() { return "multi-vitamin"; }
    const char *getDescription() { return "Only works with DSONE SDHC (SST39VF040) for now."; }

    bool initialize()
    {
        logMessage(LOG_INFO, "DSONE: Init");
        DSONE_flash_command(0x86, 0, 0);

        m_flashchip = get_flashchip_id();
        logMessage(LOG_NOTICE, "DSONE: Flashchip ID = 0x%04x", m_flashchip);
        if (!flashchip_supported(m_flashchip))
            return false;

        switch(m_flashchip) {
            case 0x9789:
                //m_cmd_type = DSONE_CMD_TYPE_2;
                break;
            default:
                m_cmd_type = DSONE_CMD_TYPE_1;
                break;
        }

        return true;
    }

    void shutdown() {
        DSONE_flash_command(0x88, 0, 0);
    }

    bool readFlash(uint32_t address, uint32_t length, uint8_t *buffer) {
        logMessage(LOG_INFO, "DSONE: readFlash(addr=0x%08x, size=0x%x)", address, length);
        DSONE_reset();

        uint32_t i = 0;
        uint32_t end_address = address + length;

        while (address < end_address)
        {
            uint32_t data = DSONE_flash_command(0, address, 0);
            showProgress(address+1, end_address, "Reading");

            buffer[i++] = (uint8_t)((data >> 0) & 0xFF);
            buffer[i++] = (uint8_t)((data >> 8) & 0xFF);
            buffer[i++] = (uint8_t)((data >> 16) & 0xFF);
            buffer[i++] = (uint8_t)((data >> 24) & 0xFF);

            address += 4;
        }

        return true;
    }

    // todo: we're just assuming this is block (0x2000) aligned
    bool writeFlash(uint32_t address, uint32_t length, const uint8_t *buffer)
    {
        // really fucking temporary, writeFlash can only do full length writes
        // todo: read and erase properly
        Erase_Chip(address);
        logMessage(LOG_INFO, "DSONE: writeFlash(addr=0x%08x, size=0x%x)", address, length);

        for(uint32_t i = 0; i < length; i++)
        {
            showProgress(i+1, length, "Writing");
            Program_Byte(address++, buffer[i]);
        }

        return true;
    }

    bool injectNtrBoot(uint8_t *blowfish_key, uint8_t *firm, uint32_t firm_size) {
        // todo: we just read and write the entire flash chip because we don't align blocks
        // properly, when writeFlash works, don't use memcpy
        logMessage(LOG_INFO, "DSONE: Injecting Ntrboot");

        // don't bother installing if we can't fit
        if (firm_size > m_max_length - 0x7E00) {
            logMessage(LOG_ERR, "DSONE: Firm too large!");
            return false; // todo: return error code
        }

        uint8_t* buffer = (uint8_t*)malloc(m_max_length);
        readFlash(0, m_max_length, buffer);

        memcpy(buffer + 0x1000, blowfish_key, 0x48);
        memcpy(buffer + 0x2000, blowfish_key + 0x48, 0x1000);
        memcpy(buffer + 0x7E00, firm, firm_size);

        writeFlash(0, m_max_length, buffer);

        return true;
    }
};

DSONE dsone;
}
