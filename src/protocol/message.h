#ifndef SHAREGRES_MESSAGE_H
#define SHAREGRES_MESSAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/*
 * PostgreSQL wire protocol message types.
 *
 * Frontend (client -> server) messages:
 */
#define MSG_QUERY           'Q'
#define MSG_PARSE           'P'
#define MSG_BIND            'B'
#define MSG_DESCRIBE        'D'
#define MSG_EXECUTE         'E'
#define MSG_SYNC            'S'
#define MSG_CLOSE           'C'
#define MSG_TERMINATE       'X'
#define MSG_COPY_DATA       'd'
#define MSG_COPY_DONE       'c'
#define MSG_COPY_FAIL       'f'
#define MSG_PASSWORD        'p'

/*
 * Backend (server -> client) messages:
 */
#define MSG_AUTH_REQUEST     'R'
#define MSG_PARAM_STATUS    'S'
#define MSG_BACKEND_KEY     'K'
#define MSG_READY_FOR_QUERY 'Z'
#define MSG_ROW_DESCRIPTION 'T'
#define MSG_DATA_ROW        'D'
#define MSG_CMD_COMPLETE    'C'
#define MSG_ERROR_RESPONSE  'E'
#define MSG_NOTICE_RESPONSE 'N'
#define MSG_PARSE_COMPLETE  '1'
#define MSG_BIND_COMPLETE   '2'
#define MSG_CLOSE_COMPLETE  '3'
#define MSG_NO_DATA         'n'
#define MSG_EMPTY_QUERY     'I'
#define MSG_COPY_IN_RESP    'G'
#define MSG_COPY_OUT_RESP   'H'
#define MSG_NOTIFICATION    'A'

/* Protocol version 3.0 */
#define PG_PROTOCOL_MAJOR   3
#define PG_PROTOCOL_MINOR   0
#define PG_PROTOCOL_VERSION ((PG_PROTOCOL_MAJOR << 16) | PG_PROTOCOL_MINOR)

/* SSL request magic number */
#define PG_SSL_REQUEST      80877103

/* Cancel request magic number */
#define PG_CANCEL_REQUEST   80877102

/* Authentication types */
#define AUTH_OK             0
#define AUTH_CLEARTEXT      3
#define AUTH_MD5            5
#define AUTH_SCRAM          10

/* Transaction status for ReadyForQuery */
#define TXN_IDLE            'I'
#define TXN_IN_TRANSACTION  'T'
#define TXN_FAILED          'E'

/*
 * Message buffer for reading/writing PG wire protocol messages.
 */
typedef struct MsgBuf {
    uint8_t    *data;
    size_t      len;        /* current data length */
    size_t      cap;        /* allocated capacity */
    size_t      pos;        /* read position */
} MsgBuf;

/* MsgBuf lifecycle */
void    msgbuf_init(MsgBuf *buf, size_t initial_cap);
void    msgbuf_reset(MsgBuf *buf);
void    msgbuf_free(MsgBuf *buf);
void    msgbuf_ensure(MsgBuf *buf, size_t additional);

/* Writing primitives */
void    msgbuf_put_byte(MsgBuf *buf, uint8_t val);
void    msgbuf_put_int16(MsgBuf *buf, int16_t val);
void    msgbuf_put_int32(MsgBuf *buf, int32_t val);
void    msgbuf_put_string(MsgBuf *buf, const char *str);
void    msgbuf_put_bytes(MsgBuf *buf, const void *data, size_t len);

/* Reading primitives */
uint8_t msgbuf_get_byte(MsgBuf *buf);
int16_t msgbuf_get_int16(MsgBuf *buf);
int32_t msgbuf_get_int32(MsgBuf *buf);
char*   msgbuf_get_string(MsgBuf *buf);  /* returns pointer into buf->data */

/*
 * Build complete backend messages (type + length-prefixed payload).
 */
void    msg_write_auth_ok(MsgBuf *buf);
void    msg_write_param_status(MsgBuf *buf, const char *name, const char *value);
void    msg_write_backend_key(MsgBuf *buf, int32_t pid, int32_t key);
void    msg_write_ready_for_query(MsgBuf *buf, char txn_status);
void    msg_write_error(MsgBuf *buf, const char *severity, const char *code,
                        const char *message);
void    msg_write_command_complete(MsgBuf *buf, const char *tag);

#endif /* SHAREGRES_MESSAGE_H */
