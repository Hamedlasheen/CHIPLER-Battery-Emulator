/* Captured SMBus framing only. No inferred iPhone register semantics or authentication. */
#ifndef CHIPLER_SMBUS_CORE_H
#define CHIPLER_SMBUS_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#define SMB_MAX_PAYLOAD 32u
#define SMB_MAX_WIRE 34u
typedef enum { SMB_OK, SMB_SHORT_A8, SMB_OFFLINE_71, SMB_UNSUPPORTED,
               SMB_BAD_LENGTH, SMB_BAD_PEC, SMB_ABORTED } smb_result_t;
typedef enum { SMB_WORD, SMB_BLOCK } smb_kind_t;
typedef struct { uint8_t addr, cmd, count, data[32]; smb_kind_t kind; uint32_t generation; } smb_image_t;
typedef struct {
    smb_image_t image[2]; atomic_uchar published;
    uint8_t wire[SMB_MAX_WIRE], wire_len, offset, latched_cmd;
    atomic_bool prepared;
    bool reading, overread; uint32_t generation, latched_generation;
    uint32_t commits, rejects, unsupported;
    uint8_t last_write_cmd, last_write_len, last_write[33]; /* opaque wire body incl block count */
} smbus_t;
uint8_t smb_crc8(const uint8_t *p, size_t n);
smb_result_t smb_validate(uint8_t addr, const uint8_t *w, size_t nw,
                          const uint8_t *r, size_t nr, bool read, bool framing_ok);
void smb_init(smbus_t *s);
/* Single core producer + IRQ consumer: publish inactive slot, latch/copy at read START.
 * Multiple producers or multicore publication are not supported by this contract. */
bool smb_publish(smbus_t *s, uint8_t addr, uint8_t cmd, smb_kind_t kind, const uint8_t *data, size_t n);
bool smb_begin_read(smbus_t *s, uint8_t addr, uint8_t cmd);
bool smb_read_byte(smbus_t *s, uint8_t *byte);
smb_result_t smb_end_read(smbus_t *s, bool master_nack, bool aborted);
smb_result_t smb_commit_write(smbus_t *s, uint8_t addr, const uint8_t *w, size_t n, bool stop);
void smb_abort(smbus_t *s);
typedef struct {
    smbus_t *target; uint8_t addr,w[35],r[34]; uint16_t nw,nr;
    bool active,read,framing,nack_seen; uint32_t last_ms;
} smb_stream_t;
void smb_stream_init(smb_stream_t *s,smbus_t *target);
void smb_stream_start(smb_stream_t *s,uint8_t address,bool read,bool repeated,bool address_ack,uint32_t now);
void smb_stream_byte(smb_stream_t *s,uint8_t byte,bool ack,uint32_t now);
smb_result_t smb_stream_stop(smb_stream_t *s,uint32_t now);
bool smb_stream_expire(smb_stream_t *s,uint32_t now,uint32_t max_idle_ms);
#endif
