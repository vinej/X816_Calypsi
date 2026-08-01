/* ==========================================================================
 * x816_sd.h -- the SD block device at $9F81-$9F8A.
 *
 * Hardware: X816_Core rtl/sd_block.sv.  Emulator: X816_Emulator src/sdblock.c.
 * Specified in X816_Core doc/MEMORY_MAP.md.
 *
 * Software never polls. The CPU is frozen for the whole transfer, so the
 * statement after a command write runs once it has completed; read SD_STATUS
 * afterwards to check for an error. That is also why busy always reads 0 --
 * the CPU cannot observe itself stalled.
 *
 * CMD AND STATUS ARE DIFFERENT ADDRESSES, and that is not cosmetic. Calypsi
 * ELIDES A VOLATILE READ that immediately follows a volatile write to the
 * SAME address, and tests the written value instead -- so a single-address
 * command/status register made every check report "error", because the
 * command value 3 has the error bit set. Verified from the listing:
 *
 *     SD_CMD = 3; return SD_CMD & 2;               -> read elided
 *     SD_CMD = 3; s = SD_CMD; return s & 2;        -> read elided
 *     SD_CMD = 3; other_io = 0; return SD_CMD & 2; -> read emitted
 *
 * Splitting the addresses removes the hazard rather than working around it.
 * ========================================================================== */

#ifndef X816_SD_H
#define X816_SD_H

#include <stdint.h>
#include <stdbool.h>

#define SD_LBA0  (*(volatile uint8_t *)0x9F81)
#define SD_LBA1  (*(volatile uint8_t *)0x9F82)
#define SD_LBA2  (*(volatile uint8_t *)0x9F83)
#define SD_LBA3  (*(volatile uint8_t *)0x9F84)
#define SD_MEM0  (*(volatile uint8_t *)0x9F85)
#define SD_MEM1  (*(volatile uint8_t *)0x9F86)
#define SD_MEM2  (*(volatile uint8_t *)0x9F87)
#define SD_COUNT (*(volatile uint8_t *)0x9F88)
#define SD_CMD    (*(volatile uint8_t *)0x9F89)   /* write only */
#define SD_STATUS (*(volatile uint8_t *)0x9F8A)   /* read only  */
#define SD_DATA   (*(volatile uint8_t *)0x9F8C)

#define SD_CMD_READ    1u   /* card -> memory, COUNT blocks, by DMA        */
#define SD_CMD_WRITE   2u   /* buffer -> card, one block                    */
#define SD_CMD_READBUF 3u   /* card -> buffer, one block, memory untouched  */
#define SD_CMD_RESET   4u   /* rewind the buffer window                     */

#define SD_ST_BUSY     0x01u
#define SD_ST_ERROR    0x02u
#define SD_ST_PRESENT  0x80u

static inline bool sd_present(void)
{
    return (SD_STATUS & SD_ST_PRESENT) != 0;
}

static inline void sd_set_lba(uint32_t lba)
{
    SD_LBA0 = (uint8_t)(lba);
    SD_LBA1 = (uint8_t)(lba >> 8);
    SD_LBA2 = (uint8_t)(lba >> 16);
    SD_LBA3 = (uint8_t)(lba >> 24);
}

/* Fetch one block into the device buffer and rewind the read window.
   Returns false if the device reported an error. */
static inline bool sd_read_buf(uint32_t lba)
{
    sd_set_lba(lba);
    SD_CMD = SD_CMD_READBUF;
    if (SD_STATUS & SD_ST_ERROR)
        return false;
    SD_CMD = SD_CMD_RESET;
    return true;
}

/* Stream `count` blocks straight into memory at a flat 24-bit address. */
static inline bool sd_read_dma(uint32_t lba, uint32_t dest, uint8_t count)
{
    sd_set_lba(lba);
    SD_MEM0  = (uint8_t)(dest);
    SD_MEM1  = (uint8_t)(dest >> 8);
    SD_MEM2  = (uint8_t)(dest >> 16);
    SD_COUNT = count;
    SD_CMD   = SD_CMD_READ;
    return (SD_STATUS & SD_ST_ERROR) == 0;
}

/* Push one byte into the device buffer at the current window position, which
 * then advances -- the same single pointer reads use. That is what makes
 * read-modify-write of a sector cheap and needs no 512-byte staging buffer in
 * RAM: fetch the sector, seek by reading, overwrite in place, send it back. */
static inline void sd_buf_put(uint8_t v)
{
    SD_DATA = v;
}

/* Send the device buffer to `lba`. The WHOLE 512-byte buffer goes, so the
 * caller must have fetched that sector first unless it is overwriting all of
 * it -- otherwise the untouched part is whatever the buffer happened to hold,
 * which is how a filesystem gets quietly shredded. */
static inline bool sd_write_buf(uint32_t lba)
{
    sd_set_lba(lba);
    SD_CMD = SD_CMD_WRITE;
    return (SD_STATUS & SD_ST_ERROR) == 0;
}

#endif /* X816_SD_H */
