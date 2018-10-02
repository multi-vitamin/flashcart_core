#include <cstring>
#include <algorithm>

#include "../device.h"

#define BIT(n) (1 << (n))

namespace flashcart_core {
using platform::logMessage;
using platform::showProgress;

class R4iSDHCHK : Flashcart {
private:
    static const uint8_t cmdGetSWRev[8];
    static const uint8_t cmdReadFlash506[8];
    static const uint8_t cmdReadFlash700[8];
    static const uint8_t cmdEraseFlash[8];
    static const uint8_t cmdWriteByteFlash[8];
    static const uint8_t cmdWaitFlashBusy[8];

    static const uint8_t cmdGetCartUniqueKey[8];
    static const uint8_t cmdUnkD0AA[8];
    static const uint8_t cmdGetChipID[8];

    static uint32_t sw_rev;
    static uint8_t Header_506[];

    uint8_t encrypt(uint8_t dec) {
        uint8_t enc = 0;
        if (dec & BIT(0)) enc |= BIT(5);
        if (dec & BIT(1)) enc |= BIT(4);
        if (dec & BIT(2)) enc |= BIT(1);
        if (dec & BIT(3)) enc |= BIT(3);
        if (dec & BIT(4)) enc |= BIT(6);
        if (dec & BIT(5)) enc |= BIT(7);
        if (dec & BIT(6)) enc |= BIT(0);
        if (dec & BIT(7)) enc |= BIT(2);
        enc ^= 0x98;
        return enc;
    }

    uint8_t decrypt(uint8_t enc) {
        uint8_t dec = 0;
        enc ^= 0x98;
        if (enc & BIT(0)) dec |= BIT(6);
        if (enc & BIT(1)) dec |= BIT(2);
        if (enc & BIT(2)) dec |= BIT(7);
        if (enc & BIT(3)) dec |= BIT(3);
        if (enc & BIT(4)) dec |= BIT(1);
        if (enc & BIT(5)) dec |= BIT(0);
        if (enc & BIT(6)) dec |= BIT(4);
        if (enc & BIT(7)) dec |= BIT(5);
        return dec;
    }

    void encrypt_memcpy(uint8_t * dst, uint8_t * src, uint32_t length) {
        for(uint32_t i = 0; i < length; ++i) {
            dst[i] = encrypt(src[i]);
        }
    }

    void read_cmd(uint32_t address, uint8_t *resp) {
        uint8_t cmdbuf[8];

        switch (sw_rev) {
            case 0x00000505:
                /*placeholder if going to be supported in the future. There are no reports that this revision currently exists.*/
                return;
            case 0x00000605:
                address = address + 0x610000;
                memcpy(cmdbuf, cmdReadFlash506, 8);
                break;
            case 0x00000007:
                case 0x00000707:
                memcpy(cmdbuf, cmdReadFlash700, 8);
                break;
            default:
                return;
        }

      cmdbuf[2] = (address >> 16) & 0x1F;
      cmdbuf[3] = (address >>  8) & 0xFF;
      cmdbuf[4] = (address >>  0) & 0xFF;

      m_card->sendCommand(cmdbuf, resp, 0x200, 80);
    }

    void wait_flash_busy(void) {
      uint8_t cmdbuf[8];
      uint32_t resp = 0;
      memcpy(cmdbuf, cmdWaitFlashBusy, 8);

      do {
          m_card->sendCommand(cmdbuf, (uint8_t *)&resp, 4, 80);
      } while(resp);
    }

    void erase_cmd(uint32_t address) {
        uint8_t cmdbuf[8];
        logMessage(LOG_DEBUG, "r4isdhc.hk: erase(0x%08x)", address);
        memcpy(cmdbuf, cmdEraseFlash, 8);
        cmdbuf[1] = (address >> 16) & 0xFF;
        cmdbuf[2] = (address >>  8) & 0xFF;
        cmdbuf[3] = (address >>  0) & 0xFF;

        m_card->sendCommand(cmdbuf, nullptr, 0, 80);
        wait_flash_busy();
    }

    void write_cmd(uint32_t address, uint8_t value) {
        uint8_t cmdbuf[8];
        logMessage(LOG_DEBUG, "r4isdhc.hk: write(0x%08x) = 0x%02x", address, value);
        memcpy(cmdbuf, cmdWriteByteFlash, 8);
        cmdbuf[1] = (address >> 16) & 0xFF;
        cmdbuf[2] = (address >>  8) & 0xFF;
        cmdbuf[3] = (address >>  0) & 0xFF;
        cmdbuf[4] = value;

        m_card->sendCommand(cmdbuf, nullptr, 0, 80);
        wait_flash_busy();
    }

    bool trySecureInit(BlowfishKey key) {
        ncgc::Err err = m_card->init();
        if (err && !err.unsupported()) {
            logMessage(LOG_ERR, "r4isdhc.hk: trySecureInit: ntrcard::init failed");
            return false;
        } else if (m_card->state() != ncgc::NTRState::Raw) {
            logMessage(LOG_ERR, "r4isdhc.hk: trySecureInit: status (%d) not RAW and cannot reset",
                static_cast<uint32_t>(m_card->state()));
            return false;
        }

        ncgc::c::ncgc_ncard_t& state = m_card->rawState();
        state.hdr.key1_romcnt = state.key1.romcnt = 0x1808F8;
        state.hdr.key2_romcnt = state.key2.romcnt = 0x416017;
        state.key2.seed_byte = 0;
        m_card->setBlowfishState(platform::getBlowfishKey(key), key != BlowfishKey::NTR);
        if ((err = m_card->beginKey1())) {
            logMessage(LOG_ERR, "r4isdhc.hk: trySecureInit: init key1 (key = %d) failed: %d", static_cast<int>(key), err.errNo());
            return false;
        }
        if ((err = m_card->beginKey2())) {
            logMessage(LOG_ERR, "r4isdhc.hk: trySecureInit: init key2 failed: %d", err.errNo());
            return false;
        }

        return true;
    }

    void injectFlash(uint32_t chunk_addr, uint32_t chunk_length, uint32_t offset, uint8_t *src, uint32_t src_length, bool encryption) {
        uint8_t *chunk = (uint8_t *)malloc(chunk_length);
        readFlash(chunk_addr, chunk_length, chunk);
        if (encryption) {
            encrypt_memcpy(chunk + offset, src, src_length);
        } else {
            memcpy(chunk + offset, src, src_length);
        }
        writeFlash(chunk_addr, chunk_length, chunk);
        free(chunk);
    }

public:
    R4iSDHCHK() : Flashcart("R4i SDHC Dual-Core", 0x200000) { }

    const char * getAuthor() {
        return
                    "Normmatt, Kitlith, stuckpixel,\n"
                    "       angelsl, EleventhSign, et al.\n";
    }
    const char * getDescription() {
        return "\n"
               "Works with several carts similar to the r4isdhc.hk\n"
               " * R4i SDHC Dual-Core (r4isdhc.hk)\n"
               " * R4i Gold (r4igold.cc)\n"
               " * R4iTT 3DS (r4itt.net)\n";
    }

    bool initialize() {
        logMessage(LOG_INFO, "r4isdhc.hk: Init");

        if (!trySecureInit(BlowfishKey::NTR) && !trySecureInit(BlowfishKey::B9Retail) && !trySecureInit(BlowfishKey::B9Dev))
        {
          logMessage(LOG_ERR, "r4isdhc.hk: Secure init failed!");
          return false;
        }

        uint32_t resp1[0x200/4];
        uint32_t resp2[0x200/4];

        //this is how the updater does it. Not sure exactly what it's for
        do {
          m_card->sendCommand(cmdGetCartUniqueKey, resp1, 0x200, 80);
          m_card->sendCommand(cmdGetCartUniqueKey, resp2, 0x200, 80);
          logMessage(LOG_DEBUG, "resp1: 0x%08x, resp2: 0x%08x", *resp1, *resp2);
        } while(std::memcmp(resp1, resp2, 0x200));

        m_card->sendCommand(cmdGetSWRev, &sw_rev, 4, 80);

        logMessage(LOG_INFO, "r4isdhc.hk: Current Software Revision: %08x", sw_rev);

        m_card->sendCommand(cmdUnkD0AA, nullptr, 4, 80);
        m_card->sendCommand(cmdUnkD0AA, nullptr, 4, 80);
        m_card->sendCommand(cmdGetChipID, nullptr, 0, 80);
        m_card->sendCommand(cmdUnkD0AA, nullptr, 4, 80);

        do {
          m_card->sendCommand(cmdGetCartUniqueKey, resp1, 0x200, 80);
          m_card->sendCommand(cmdGetCartUniqueKey, resp2, 0x200, 80);
        } while(std::memcmp(resp1, resp2, 0x200));
        
        switch (sw_rev) {
            case 0x00000505:
                logMessage(LOG_ERR, "r4isdhc.hk: Anything below 0x00000605 is not supported.");
                return false;
            case 0x00000605:
            case 0x00000007:
            case 0x00000707:
                break;
            default:
                return false;
        }

        return true;
    }
 
    void shutdown() {
        logMessage(LOG_INFO, "r4isdhc.hk: Shutdown");
    }

    bool readFlash(uint32_t address, uint32_t length, uint8_t *buffer) {
        logMessage(LOG_INFO, "r4isdhc.hk: readFlash(addr=0x%08x, size=0x%x)", address, length);
        for(uint32_t addr = 0; addr < length; addr += 0x200)
        {
            read_cmd(addr + address, buffer + addr);
            showProgress(addr, length, "Reading");
            for(int i = 0; i < 0x200; i++)
                /*the read command decrypts the raw flash contents before returning it you*/
                /*so to get the raw flash contents, encrypt the returned values*/
                *(buffer + addr + i) = encrypt(*(buffer + addr + i));
        }
        return true;
    }

    bool writeFlash(uint32_t address, uint32_t length, const uint8_t *buffer) {
        logMessage(LOG_INFO, "r4isdhc.hk: writeFlash(addr=0x%08x, size=0x%x)", address, length);
        for (uint32_t addr=0; addr < length; addr+=0x10000) {
           erase_cmd(address + addr);
           showProgress(addr, length, "Erasing");
        }
        
        for (uint32_t i=0; i < length; i++) {
            /*the write command encrypts whatever you send it before actually writing to flash*/
            /*so we decrypt whatever we send to be written*/
            uint8_t byte = decrypt(buffer[i]);
            write_cmd(address + i, byte);
            showProgress(i,length, "Writing");
        }

        return true;
    }

    bool injectNtrBoot(uint8_t *blowfish_key, uint8_t *firm, uint32_t firm_size) {
        logMessage(LOG_INFO, "r4isdhc.hk: Injecting ntrboot");

        switch (sw_rev) {
            case 0x00000505:
                /*placeholder if going to be supported in the future. There are no reports that this revision currently exists.*/
                return false;
            case 0x00000605:
                break;
            case 0x00000007:
            case 0x00000707:
                logMessage(LOG_INFO, "r4isdhc.hk: Write firmware (header) revision 5.06");
                injectFlash(0x000000, 0x10000, 0x000000, Header_506, 0x000984, false);                  //cart header
                break;
            default:
                logMessage(LOG_ERR, "r4isdhc.hk: 0x%08x is not a recognized version and is therefore not supported.", sw_rev);
                return false;
        }

        injectFlash(0x010000, 0x10000, 0x000000, blowfish_key, 0x001048, true);                         //blowfish 1
        injectFlash(0x010000, 0x10000, 0x0055A8, firm, 0x200, true);                                    //FIRM header
        uint32_t buf_size = PAGE_ROUND_UP(firm_size - 0x200 + 0x000000, 0x10000);
        injectFlash(0x030000, buf_size, 0x000000, firm + 0x200, firm_size - 0x200, true);               //FIRM body

        return true;
    }
};

const uint8_t R4iSDHCHK::cmdGetCartUniqueKey[8] = {0xB7, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00};     //reads flash at offset 0x2FE00
const uint8_t R4iSDHCHK::cmdUnkD0AA[8] = {0xD0, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t R4iSDHCHK::cmdGetChipID[8] = {0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};            //returns as 0xFC2

const uint8_t R4iSDHCHK::cmdGetSWRev[8] = {0xC5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t R4iSDHCHK::cmdReadFlash700[8] = {0xB7, 0x00, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00};
const uint8_t R4iSDHCHK::cmdReadFlash506[8] = {0xB7, 0x01, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t R4iSDHCHK::cmdEraseFlash[8] = {0xD4, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
const uint8_t R4iSDHCHK::cmdWriteByteFlash[8] = {0xD4, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00};
const uint8_t R4iSDHCHK::cmdWaitFlashBusy[8] = {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

uint32_t R4iSDHCHK::sw_rev = 0;

uint8_t R4iSDHCHK::Header_506[] = {                            //stock 506 header
    0x8B, 0x40, 0x03, 0x11, 0x4F, 0x00, 0x10, 0xAF, 
    0x00, 0x01, 0x50, 0x03, 0x2F, 0xCF, 0x02, 0x00, 
    0xA0, 0x02, 0x1D, 0x0F, 0x00, 0x01, 0xCF, 0x01, 
    0x07, 0x54, 0x03, 0x00, 0xA0, 0x02, 0x5F, 0x00, 
    0x00, 0x01, 0xC0, 0x01, 0x0B, 0x54, 0x03, 0x00, 
    0xA0, 0x02, 0x06, 0xCE, 0x02, 0x06, 0xCD, 0x02, 
    0x06, 0xCC, 0x02, 0x06, 0xCB, 0x02, 0x06, 0xCA, 
    0x02, 0x07, 0xC2, 0x02, 0x00, 0xA0, 0x02, 0x11, 
    0x4F, 0x00, 0x01, 0xAF, 0x00, 0x15, 0x50, 0x03, 
    0x00, 0xA0, 0x02, 0x11, 0x4F, 0x00, 0x01, 0xAF, 
    0x00, 0x19, 0x54, 0x03, 0x00, 0xA0, 0x02, 0x15, 
    0x00, 0x03, 0x17, 0x4F, 0x00, 0x40, 0xAF, 0x00, 
    0x1E, 0x50, 0x03, 0x37, 0x43, 0x00, 0x36, 0x42, 
    0x00, 0x35, 0x41, 0x00, 0x34, 0x40, 0x00, 0x33, 
    0x47, 0x00, 0x32, 0x46, 0x00, 0x31, 0x45, 0x00, 
    0x30, 0x44, 0x00, 0x00, 0xA0, 0x02, 0x15, 0x00, 
    0x03, 0x11, 0x4F, 0x00, 0x80, 0xAF, 0x00, 0x2B, 
    0x50, 0x03, 0x37, 0x43, 0x00, 0x36, 0x42, 0x00, 
    0x35, 0x41, 0x00, 0x34, 0x40, 0x00, 0x33, 0x47, 
    0x00, 0x32, 0x46, 0x00, 0x31, 0x45, 0x00, 0x30, 
    0x44, 0x00, 0x00, 0xA0, 0x02, 0x3B, 0x0F, 0x00, 
    0x04, 0xCF, 0x02, 0x10, 0x1F, 0x00, 0x04, 0xCF, 
    0x02, 0x00, 0x1F, 0x00, 0x04, 0xCF, 0x02, 0x70, 
    0x1F, 0x00, 0x04, 0xCF, 0x02, 0x00, 0x0F, 0x00, 
    0x04, 0xCF, 0x02, 0x05, 0xCF, 0x02, 0x00, 0xA0, 
    0x02, 0x1D, 0x00, 0x03, 0xB7, 0x43, 0x01, 0x43, 
    0x54, 0x03, 0x00, 0xA0, 0x02, 0x1D, 0x00, 0x03, 
    0xD5, 0x43, 0x01, 0x47, 0x54, 0x03, 0x00, 0xA0, 
    0x02, 0x00, 0x00, 0x00, 0x15, 0x00, 0x03, 0x19, 
    0x00, 0x03, 0x15, 0x00, 0x03, 0x01, 0x80, 0x01, 
    0x40, 0x40, 0x01, 0x4D, 0x54, 0x03, 0x00, 0xA0, 
    0x02, 0x17, 0x4F, 0x00, 0x01, 0xAF, 0x00, 0x53, 
    0x50, 0x03, 0x00, 0xA0, 0x02, 0x06, 0x0F, 0x00, 
    0x04, 0xCF, 0x02, 0x00, 0x0F, 0x00, 0x04, 0xCF, 
    0x02, 0x00, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x00, 
    0x0F, 0x00, 0x04, 0xCF, 0x02, 0x00, 0x0F, 0x00, 
    0x04, 0xCF, 0x02, 0x24, 0xCF, 0x02, 0x01, 0x00, 
    0x03, 0x00, 0xA0, 0x02, 0x57, 0x00, 0x03, 0xD8, 
    0x0F, 0x00, 0x04, 0xCF, 0x02, 0x20, 0x1F, 0x00, 
    0x04, 0xCF, 0x02, 0x00, 0x0F, 0x00, 0x04, 0xCF, 
    0x02, 0x00, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x00, 
    0x0F, 0x00, 0x04, 0xCF, 0x02, 0x21, 0xCF, 0x02, 
    0x0A, 0x00, 0x03, 0x01, 0x00, 0x03, 0x00, 0xA0, 
    0x02, 0x05, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x00, 
    0x0F, 0x00, 0x04, 0xCF, 0x02, 0x00, 0x0F, 0x00, 
    0x04, 0xCF, 0x02, 0x00, 0x0F, 0x00, 0x04, 0xCF, 
    0x02, 0x00, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x01, 
    0xCF, 0x02, 0x01, 0x00, 0x03, 0x10, 0x4F, 0x00, 
    0x02, 0xAF, 0x00, 0x73, 0x54, 0x03, 0x2E, 0xCF, 
    0x02, 0x00, 0xA0, 0x02, 0x28, 0xCF, 0x02, 0x18, 
    0x4F, 0x00, 0x00, 0xA0, 0x02, 0x17, 0x4F, 0x00, 
    0x40, 0xAF, 0x00, 0x87, 0x50, 0x03, 0x00, 0xA0, 
    0x02, 0x1D, 0x00, 0x03, 0x00, 0x43, 0x01, 0x8B, 
    0x54, 0x03, 0x3B, 0x0F, 0x00, 0x04, 0xCF, 0x02, 
    0x01, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x00, 0x0F, 
    0x00, 0x11, 0x0E, 0x00, 0xE0, 0x9F, 0x01, 0x04, 
    0xCF, 0x02, 0x00, 0x0F, 0x00, 0x04, 0xCF, 0x02, 
    0x00, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x05, 0xCF, 
    0x02, 0x2B, 0xCF, 0x02, 0x15, 0x00, 0x03, 0x2F, 
    0xCF, 0x02, 0x19, 0x00, 0x03, 0x87, 0x00, 0x03, 
    0x37, 0x4F, 0x00, 0x3C, 0x4F, 0x01, 0x9C, 0x54, 
    0x03, 0x2A, 0xCF, 0x02, 0x44, 0x0E, 0x00, 0x2A, 
    0x00, 0x03, 0x3B, 0x0F, 0x00, 0x04, 0xCF, 0x02, 
    0x01, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x00, 0x0F, 
    0x00, 0x04, 0xCF, 0x02, 0x04, 0xCE, 0x02, 0x00, 
    0x0F, 0x00, 0x04, 0xCF, 0x02, 0x26, 0xCF, 0x02, 
    0x01, 0x00, 0x03, 0x14, 0x4F, 0x00, 0xF0, 0xF0, 
    0x00, 0x13, 0x4F, 0x00, 0xF0, 0xF1, 0x00, 0x16, 
    0x4F, 0x00, 0xF0, 0xF2, 0x00, 0x15, 0x4F, 0x00, 
    0xF0, 0xF3, 0x00, 0x10, 0x1F, 0x00, 0x30, 0x11, 
    0x00, 0xF0, 0x13, 0x00, 0x00, 0x1F, 0x00, 0x20, 
    0x10, 0x00, 0xF0, 0x12, 0x00, 0x00, 0x09, 0x00, 
    0x10, 0x18, 0x00, 0x06, 0x08, 0x02, 0x00, 0x09, 
    0x02, 0x06, 0x08, 0x02, 0x00, 0x09, 0x02, 0x48, 
    0x88, 0x01, 0x00, 0xA9, 0x01, 0x3B, 0x0F, 0x00, 
    0x04, 0xCF, 0x02, 0x01, 0x0F, 0x00, 0x04, 0xCF, 
    0x02, 0x00, 0x0F, 0x00, 0x90, 0x9F, 0x01, 0x04, 
    0xCF, 0x02, 0x04, 0xC8, 0x02, 0x00, 0x0F, 0x00, 
    0x04, 0xCF, 0x02, 0x26, 0xCF, 0x02, 0x00, 0x09, 
    0x00, 0x00, 0x18, 0x00, 0x06, 0x08, 0x02, 0x00, 
    0x09, 0x02, 0x01, 0x00, 0x03, 0x14, 0x4A, 0x00, 
    0x13, 0x4B, 0x00, 0x16, 0x4C, 0x00, 0x15, 0x4D, 
    0x00, 0x06, 0x08, 0x02, 0x00, 0x09, 0x02, 0x48, 
    0x88, 0x01, 0x04, 0xA9, 0x01, 0x10, 0x1F, 0x00, 
    0x30, 0x11, 0x00, 0xF0, 0x13, 0x00, 0x3B, 0x0F, 
    0x00, 0x04, 0xCF, 0x02, 0x01, 0x0F, 0x00, 0x04, 
    0xCF, 0x02, 0x00, 0x0F, 0x00, 0x90, 0x9F, 0x01, 
    0x04, 0xCF, 0x02, 0x04, 0xC8, 0x02, 0x00, 0x0F, 
    0x00, 0x04, 0xCF, 0x02, 0x26, 0xCF, 0x02, 0x01, 
    0x00, 0x03, 0x14, 0x4F, 0x00, 0xF0, 0x9A, 0x01, 
    0x13, 0x4F, 0x00, 0xF0, 0xBB, 0x01, 0x16, 0x4F, 
    0x00, 0xF0, 0xBC, 0x01, 0x15, 0x4F, 0x00, 0xF0, 
    0xBD, 0x01, 0x00, 0x1F, 0x00, 0x20, 0x10, 0x00, 
    0xF0, 0x12, 0x00, 0x00, 0x09, 0x00, 0x10, 0x18, 
    0x00, 0x06, 0x08, 0x02, 0x00, 0x09, 0x02, 0x06, 
    0x08, 0x02, 0x00, 0x09, 0x02, 0x48, 0x88, 0x01, 
    0x08, 0xA9, 0x01, 0x3B, 0x0F, 0x00, 0x04, 0xCF, 
    0x02, 0x01, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x00, 
    0x0F, 0x00, 0x90, 0x9F, 0x01, 0x04, 0xCF, 0x02, 
    0x04, 0xC8, 0x02, 0x00, 0x0F, 0x00, 0x04, 0xCF, 
    0x02, 0x26, 0xCF, 0x02, 0x01, 0x00, 0x03, 0x00, 
    0x09, 0x00, 0x00, 0x18, 0x00, 0x14, 0x4F, 0x00, 
    0xF0, 0xFA, 0x00, 0x13, 0x4F, 0x00, 0xF0, 0xFB, 
    0x00, 0x16, 0x4F, 0x00, 0xF0, 0xFC, 0x00, 0x15, 
    0x4F, 0x00, 0xF0, 0xFD, 0x00, 0x06, 0x08, 0x02, 
    0x00, 0x09, 0x02, 0x06, 0x08, 0x02, 0x00, 0x09, 
    0x02, 0x48, 0x88, 0x01, 0x0C, 0xA9, 0x01, 0x3B, 
    0x0F, 0x00, 0x04, 0xCF, 0x02, 0x01, 0x0F, 0x00, 
    0x04, 0xCF, 0x02, 0x00, 0x0F, 0x00, 0x90, 0x9F, 
    0x01, 0x04, 0xCF, 0x02, 0x04, 0xC8, 0x02, 0x00, 
    0x0F, 0x00, 0x04, 0xCF, 0x02, 0x26, 0xCF, 0x02, 
    0x01, 0x00, 0x03, 0x00, 0x0F, 0x00, 0x14, 0x4F, 
    0x00, 0xF0, 0x9A, 0x01, 0x13, 0x4F, 0x00, 0xF0, 
    0xBB, 0x01, 0x16, 0x4F, 0x00, 0xF0, 0xBC, 0x01, 
    0x15, 0x4F, 0x00, 0xF0, 0xBD, 0x01, 0xA0, 0xF4, 
    0x00, 0xB0, 0xF5, 0x00, 0xC0, 0xF6, 0x00, 0xD0, 
    0xF7, 0x00, 0x40, 0x1F, 0x00, 0x00, 0x14, 0x00, 
    0xF0, 0x10, 0x00, 0x50, 0x1F, 0x00, 0x10, 0x15, 
    0x00, 0xF0, 0x11, 0x00, 0x60, 0x1F, 0x00, 0x20, 
    0x16, 0x00, 0xF0, 0x12, 0x00, 0x70, 0x1F, 0x00, 
    0x30, 0x17, 0x00, 0xF0, 0x13, 0x00, 0x04, 0xCE, 
    0x01, 0x00, 0x0F, 0x00, 0x3B, 0x0F, 0x00, 0x04, 
    0xCF, 0x02, 0x01, 0x0F, 0x00, 0x04, 0xCF, 0x02, 
    0x00, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x04, 0xCE, 
    0x02, 0x00, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x26, 
    0xCF, 0x02, 0x01, 0x00, 0x03, 0x00, 0x0F, 0x00, 
    0x14, 0x4F, 0x00, 0xF0, 0xF0, 0x00, 0x13, 0x4F, 
    0x00, 0xF0, 0xF1, 0x00, 0x16, 0x4F, 0x00, 0xF0, 
    0xF2, 0x00, 0x15, 0x4F, 0x00, 0xF0, 0xF3, 0x00, 
    0x04, 0x4E, 0x01, 0xB9, 0x54, 0x03, 0x04, 0xCE, 
    0x01, 0x00, 0x0F, 0x00, 0x3B, 0x0F, 0x00, 0x04, 
    0xCF, 0x02, 0x01, 0x0F, 0x00, 0x04, 0xCF, 0x02, 
    0x00, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x04, 0xCE, 
    0x02, 0x00, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x26, 
    0xCF, 0x02, 0x01, 0x00, 0x03, 0x00, 0x0F, 0x00, 
    0x14, 0x4F, 0x00, 0xF0, 0xF4, 0x00, 0x13, 0x4F, 
    0x00, 0xF0, 0xF5, 0x00, 0x16, 0x4F, 0x00, 0xF0, 
    0xF6, 0x00, 0x15, 0x4F, 0x00, 0xF0, 0xF7, 0x00, 
    0x40, 0x1F, 0x00, 0x00, 0x14, 0x00, 0xF0, 0x10, 
    0x00, 0x50, 0x1F, 0x00, 0x10, 0x15, 0x00, 0xF0, 
    0x11, 0x00, 0x60, 0x1F, 0x00, 0x20, 0x16, 0x00, 
    0xF0, 0x12, 0x00, 0x70, 0x1F, 0x00, 0x30, 0x17, 
    0x00, 0xF0, 0x13, 0x00, 0x00, 0x0F, 0x00, 0x30, 
    0x1E, 0x00, 0xF0, 0xAE, 0x00, 0x10, 0x4E, 0x01, 
    0x83, 0x55, 0x03, 0x01, 0x0F, 0x00, 0x8A, 0x41, 
    0x03, 0x20, 0x4E, 0x01, 0x87, 0x55, 0x03, 0x02, 
    0x0F, 0x00, 0x8A, 0x41, 0x03, 0xA0, 0x4E, 0x01, 
    0x8A, 0x55, 0x03, 0x10, 0x0F, 0x00, 0x03, 0xCF, 
    0x02, 0x02, 0xC3, 0x02, 0x02, 0xC2, 0x02, 0x02, 
    0xC1, 0x02, 0x02, 0xC0, 0x02, 0x02, 0xC7, 0x02, 
    0x02, 0xC6, 0x02, 0x02, 0xC5, 0x02, 0x02, 0xC4, 
    0x02, 0x01, 0x0F, 0x00, 0x00, 0xCF, 0x02, 0x00, 
    0x0F, 0x00, 0x00, 0xCF, 0x02, 0x11, 0x4F, 0x00, 
    0xF0, 0x1A, 0x00, 0x01, 0xAF, 0x00, 0xBD, 0x55, 
    0x03, 0x40, 0xAA, 0x00, 0x97, 0x51, 0x03, 0x10, 
    0x1F, 0x00, 0xF0, 0xAF, 0x00, 0x40, 0x4F, 0x01, 
    0xA8, 0x51, 0x03, 0x50, 0x4F, 0x01, 0xAB, 0x51, 
    0x03, 0x60, 0x4F, 0x01, 0xAE, 0x51, 0x03, 0x01, 
    0x01, 0x00, 0x47, 0x00, 0x00, 0xB9, 0x41, 0x03, 
    0x01, 0x01, 0x00, 0x14, 0x00, 0x00, 0xB9, 0x41, 
    0x03, 0x01, 0x01, 0x00, 0x25, 0x00, 0x00, 0xB9, 
    0x41, 0x03, 0x01, 0x01, 0x00, 0x36, 0x00, 0x00, 
    0xB9, 0x41, 0x03, 0x10, 0x1F, 0x00, 0xF0, 0xAF, 
    0x00, 0x40, 0x4F, 0x01, 0xB7, 0x51, 0x03, 0x0C, 
    0xCF, 0x02, 0xBB, 0x41, 0x03, 0x01, 0x01, 0x00, 
    0x6C, 0x00, 0x00, 0x00, 0x07, 0x00, 0x37, 0x00, 
    0x03, 0x15, 0x00, 0x03, 0x2F, 0xCF, 0x02, 0xA0, 
    0x4E, 0x01, 0xA4, 0x54, 0x03, 0x44, 0x0F, 0x00, 
    0x2C, 0xCF, 0x02, 0x55, 0x0F, 0x00, 0x05, 0xEF, 
    0x02, 0x06, 0xEF, 0x02, 0x15, 0x00, 0x03, 0x2F, 
    0xCF, 0x02, 0x17, 0x4F, 0x00, 0x80, 0xAF, 0x00, 
    0xC6, 0x51, 0x03, 0x37, 0x43, 0x00, 0xC7, 0x43, 
    0x01, 0xD5, 0x51, 0x03, 0xD1, 0x43, 0x01, 0xDE, 
    0x51, 0x03, 0xD0, 0x43, 0x01, 0xE7, 0x51, 0x03, 
    0xC2, 0x43, 0x01, 0xE7, 0x51, 0x03, 0xC5, 0x43, 
    0x01, 0xF1, 0x51, 0x03, 0xFA, 0x41, 0x03, 0xB5, 
    0x0F, 0x00, 0x22, 0xCF, 0x02, 0x5B, 0x0F, 0x00, 
    0x22, 0xCF, 0x02, 0xDB, 0x0F, 0x00, 0x22, 0xCF, 
    0x02, 0xB7, 0x0F, 0x00, 0x22, 0xCF, 0x02, 0xC4, 
    0x41, 0x03, 0x80, 0x0F, 0x00, 0x22, 0xCF, 0x02, 
    0x80, 0x0F, 0x00, 0x22, 0xCF, 0x02, 0x80, 0x0F, 
    0x00, 0x22, 0xCF, 0x02, 0x80, 0x0F, 0x00, 0x22, 
    0xCF, 0x02, 0xC4, 0x41, 0x03, 0xC2, 0x0F, 0x00, 
    0x22, 0xCF, 0x02, 0x0F, 0x0F, 0x00, 0x22, 0xCF, 
    0x02, 0x00, 0x0F, 0x00, 0x22, 0xCF, 0x02, 0x00, 
    0x0F, 0x00, 0x22, 0xCF, 0x02, 0x36, 0x42, 0x00, 
    0xF0, 0x42, 0x03, 0x05, 0x0F, 0x00, 0x22, 0xCF, 
    0x02, 0x06, 0x0F, 0x00, 0x22, 0xCF, 0x02, 0x00, 
    0x0F, 0x00, 0x22, 0xCF, 0x02, 0x00, 0x0F, 0x00, 
    0x22, 0xCF, 0x02, 0xC4, 0x41, 0x03, 0x11, 0x4F, 
    0x00, 0x80, 0xAF, 0x00, 0xFA, 0x51, 0x03, 0x37, 
    0x43, 0x00, 0x36, 0x42, 0x00, 0x35, 0x41, 0x00, 
    0x34, 0x40, 0x00, 0x33, 0x47, 0x00, 0x32, 0x46, 
    0x00, 0x31, 0x45, 0x00, 0x30, 0x44, 0x00, 0xB7, 
    0x43, 0x01, 0x55, 0x56, 0x03, 0x01, 0x42, 0x01, 
    0x0F, 0x56, 0x03, 0xE5, 0x41, 0x01, 0x25, 0x52, 
    0x03, 0x61, 0xC1, 0x01, 0xAA, 0x0F, 0x00, 0x05, 
    0xEF, 0x02, 0x3D, 0x42, 0x03, 0x12, 0x46, 0x01, 
    0x28, 0x52, 0x03, 0x10, 0x46, 0x01, 0x28, 0x52, 
    0x03, 0x0A, 0x44, 0x01, 0x49, 0x52, 0x03, 0x17, 
    0x41, 0x01, 0x1A, 0x5E, 0x03, 0x80, 0x80, 0x01, 
    0x02, 0xA1, 0x01, 0x3D, 0x42, 0x03, 0x23, 0x56, 
    0x03, 0x05, 0x6F, 0x00, 0xAA, 0x4F, 0x01, 0x23, 
    0x56, 0x03, 0x90, 0x40, 0x01, 0x23, 0x5A, 0x03, 
    0xD0, 0xC0, 0x01, 0x01, 0xE1, 0x01, 0x3D, 0x42, 
    0x03, 0x01, 0xC1, 0x01, 0x3D, 0x42, 0x03, 0x02, 
    0x01, 0x00, 0xF0, 0x00, 0x00, 0x3D, 0x42, 0x03, 
    0x00, 0x41, 0x01, 0x2F, 0x56, 0x03, 0x20, 0x40, 
    0x01, 0x2F, 0x56, 0x03, 0x01, 0x01, 0x00, 0x69, 
    0x00, 0x00, 0x3D, 0x42, 0x03, 0x04, 0x41, 0x01, 
    0x3D, 0x52, 0x03, 0x05, 0x41, 0x01, 0x3D, 0x52, 
    0x03, 0x06, 0x41, 0x01, 0x3D, 0x52, 0x03, 0x07, 
    0x41, 0x01, 0x3D, 0x52, 0x03, 0x02, 0x41, 0x01, 
    0x3C, 0x52, 0x03, 0x20, 0x80, 0x01, 0x01, 0xA1, 
    0x01, 0x3D, 0x42, 0x03, 0xA0, 0x80, 0x01, 0x3B, 
    0x0F, 0x00, 0x04, 0xCF, 0x02, 0x10, 0x1F, 0x00, 
    0x04, 0xCF, 0x02, 0x00, 0x1F, 0x00, 0x04, 0xCF, 
    0x02, 0x70, 0x1F, 0x00, 0x04, 0xCF, 0x02, 0x00, 
    0x0F, 0x00, 0x04, 0xCF, 0x02, 0x05, 0xCF, 0x02, 
    0xC4, 0x41, 0x03, 0xE0, 0x0F, 0x00, 0x22, 0xCF, 
    0x02, 0x22, 0xCF, 0x02, 0x22, 0xCF, 0x02, 0x22, 
    0xCF, 0x02, 0xC4, 0x41, 0x03, 0x0B, 0x0F, 0x00, 
    0x22, 0xCF, 0x02, 0x22, 0xCF, 0x02, 0x22, 0xCF, 
    0x02, 0x22, 0xCF, 0x02, 0xC4, 0x41, 0x03, 0xD5, 
    0x43, 0x01, 0xE6, 0x56, 0x03, 0x17, 0x4F, 0x00, 
    0x02, 0xAF, 0x00, 0x5C, 0x52, 0x03, 0x19, 0x40, 
    0x01, 0xD8, 0x52, 0x03, 0x20, 0x1F, 0x00, 0xF0, 
    0xAF, 0x00, 0x20, 0x4F, 0x01, 0x62, 0x56, 0x03, 
    0x03, 0x08, 0x00, 0x65, 0x42, 0x03, 0x10, 0x4F, 
    0x01, 0xA6, 0x56, 0x03, 0x04, 0x08, 0x00, 0x70, 
    0x1D, 0x00, 0x60, 0x1C, 0x00, 0x50, 0x1B, 0x00, 
    0x0E, 0x0D, 0x02, 0x08, 0x0C, 0x02, 0x08, 0x0B, 
    0x02, 0x50, 0x1A, 0x00, 0x07, 0xAA, 0x00, 0x02, 
    0x49, 0x01, 0x89, 0x52, 0x03, 0x0E, 0x0D, 0x02, 
    0x08, 0x0C, 0x02, 0x08, 0x0B, 0x02, 0x50, 0x1A, 
    0x00, 0x0F, 0xAA, 0x00, 0x03, 0x49, 0x01, 0x89, 
    0x52, 0x03, 0x0E, 0x0D, 0x02, 0x08, 0x0C, 0x02, 
    0x08, 0x0B, 0x02, 0x50, 0x1A, 0x00, 0x1F, 0xAA, 
    0x00, 0x04, 0x49, 0x01, 0x89, 0x52, 0x03, 0x0E, 
    0x0D, 0x02, 0x08, 0x0C, 0x02, 0x08, 0x0B, 0x02, 
    0x50, 0x1A, 0x00, 0x3F, 0xAA, 0x00, 0x05, 0x49, 
    0x01, 0x89, 0x52, 0x03, 0x0E, 0x0D, 0x02, 0x08, 
    0x0C, 0x02, 0x08, 0x0B, 0x02, 0x50, 0x1A, 0x00, 
    0x7F, 0xAA, 0x00, 0xA0, 0x15, 0x00, 0xFC, 0xAB, 
    0x00, 0x3B, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x80, 
    0x1F, 0x00, 0xD0, 0x9F, 0x01, 0x04, 0xCF, 0x02, 
    0x04, 0xCC, 0x02, 0x04, 0xCB, 0x02, 0x00, 0x0F, 
    0x00, 0x04, 0xCF, 0x02, 0x26, 0xCF, 0x02, 0x01, 
    0x00, 0x03, 0x14, 0x4A, 0x00, 0x13, 0x4B, 0x00, 
    0x16, 0x4C, 0x00, 0x15, 0x4D, 0x00, 0x17, 0x4F, 
    0x00, 0x08, 0xAF, 0x00, 0xA0, 0x56, 0x03, 0xA0, 
    0x14, 0x00, 0xB0, 0x95, 0x01, 0xA4, 0x42, 0x03, 
    0x0E, 0x05, 0x02, 0x50, 0x9A, 0x01, 0xA0, 0x14, 
    0x00, 0xB0, 0x15, 0x00, 0xC0, 0x16, 0x00, 0xD0, 
    0x17, 0x00, 0x00, 0x1E, 0x00, 0x70, 0x1D, 0x00, 
    0x60, 0x1C, 0x00, 0x50, 0x1B, 0x00, 0x40, 0x1A, 
    0x00, 0x0E, 0x00, 0x03, 0x11, 0x4E, 0x01, 0xB2, 
    0x56, 0x03, 0x43, 0x00, 0x03, 0x15, 0x00, 0x03, 
    0x0B, 0xCF, 0x02, 0xC4, 0x41, 0x03, 0x12, 0x4E, 
    0x01, 0xCD, 0x56, 0x03, 0x1D, 0x00, 0x03, 0xB7, 
    0x43, 0x01, 0xBB, 0x52, 0x03, 0xD5, 0x43, 0x01, 
    0xB4, 0x56, 0x03, 0x0B, 0xCF, 0x02, 0x5C, 0x42, 
    0x03, 0x12, 0x46, 0x01, 0xBF, 0x52, 0x03, 0x10, 
    0x46, 0x01, 0xCA, 0x56, 0x03, 0x09, 0xCF, 0x02, 
    0x37, 0x00, 0x03, 0x0B, 0xCF, 0x02, 0x0C, 0x0E, 
    0x00, 0x00, 0x0D, 0x00, 0x00, 0x0C, 0x00, 0x00, 
    0x0B, 0x00, 0x00, 0x0A, 0x00, 0x01, 0x02, 0x00, 
    0x0E, 0x00, 0x03, 0xB4, 0x42, 0x03, 0x15, 0x00, 
    0x03, 0x0A, 0xCF, 0x02, 0xB4, 0x42, 0x03, 0x18, 
    0x4E, 0x01, 0xD6, 0x56, 0x03, 0x15, 0x00, 0x03, 
    0x0D, 0xCF, 0x02, 0x4B, 0x00, 0x03, 0x0A, 0x00, 
    0x03, 0x53, 0x00, 0x03, 0x0E, 0xCF, 0x02, 0xC4, 
    0x41, 0x03, 0x19, 0x4E, 0x01, 0xDF, 0x56, 0x03, 
    0x15, 0x00, 0x03, 0x0F, 0xCF, 0x02, 0x0F, 0xCF, 
    0x02, 0x0F, 0xCF, 0x02, 0x0D, 0xCF, 0x02, 0x4B, 
    0x00, 0x03, 0xC4, 0x41, 0x03, 0x0C, 0x4E, 0x01, 
    0xC4, 0x55, 0x03, 0x0A, 0x00, 0x03, 0x53, 0x00, 
    0x03, 0x0B, 0xCF, 0x02, 0x0E, 0xCF, 0x02, 0xC4, 
    0x41, 0x03, 0xB8, 0x43, 0x01, 0xC4, 0x51, 0x03, 
    0xD7, 0x43, 0x01, 0xF5, 0x52, 0x03, 0xD4, 0x43, 
    0x01, 0xFB, 0x52, 0x03, 0xC0, 0x43, 0x01, 0xC4, 
    0x51, 0x03, 0x2D, 0xCF, 0x02, 0xC4, 0x41, 0x03, 
    0xAA, 0x42, 0x01, 0xC4, 0x55, 0x03, 0xAA, 0x0F, 
    0x00, 0x06, 0xEF, 0x02, 0xC4, 0x41, 0x03, 0x25, 
    0xCF, 0x02, 0x50, 0x19, 0x00, 0x80, 0xA5, 0x00, 
    0xC4, 0x51, 0x03, 0x23, 0xCF, 0x02, 0xC4, 0x41, 
    0x03, 0x06, 0x6F, 0x00, 0xAA, 0x4F, 0x01, 0xC4, 
    0x55, 0x03, 0x01, 0x46, 0x01, 0x03, 0x53, 0x03, 
    0x03, 0x46, 0x01, 0x08, 0x53, 0x03, 0xC4, 0x41, 
    0x03, 0x08, 0xCF, 0x02, 0x64, 0x00, 0x03, 0x73, 
    0x00, 0x03, 0x08, 0xCF, 0x02, 0xC4, 0x41, 0x03, 
    0x08, 0xCF, 0x02, 0x57, 0x00, 0x03, 0x02, 0x0F, 
    0x00, 0x04, 0xCF, 0x02, 0x20, 0x1F, 0x00, 0x04, 
    0xCF, 0x02, 0x10, 0x1F, 0x00, 0x04, 0xCF, 0x02, 
    0x00, 0x1F, 0x00, 0x04, 0xCF, 0x02, 0x70, 0x1F, 
    0x00, 0x84, 0x00, 0x03, 0x04, 0xCF, 0x02, 0x20, 
    0xCF, 0x02, 0x01, 0x00, 0x03, 0x0A, 0x00, 0x03, 
    0x73, 0x00, 0x03, 0x08, 0xCF, 0x02, 0x03, 0x42, 
    0x01, 0x1D, 0x53, 0x03, 0x04, 0x02, 0x00, 0x3B, 
    0x0F, 0x00, 0x04, 0xCF, 0x02, 0x20, 0x1F, 0x00, 
    0x04, 0xCF, 0x02, 0x00, 0x0F, 0x00, 0x04, 0xCF, 
    0x02, 0x00, 0x0F, 0x00, 0x04, 0xCF, 0x02, 0x00, 
    0x0F, 0x00, 0x04, 0xCF, 0x02, 0xC4, 0x41, 0x03, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

R4iSDHCHK r4isdhchk;

}