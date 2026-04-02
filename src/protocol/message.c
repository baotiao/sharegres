#include "message.h"
#include <string.h>
#include <arpa/inet.h>

void msgbuf_init(MsgBuf *buf, size_t initial_cap)
{
    buf->data = malloc(initial_cap);
    buf->len = 0;
    buf->cap = initial_cap;
    buf->pos = 0;
}

void msgbuf_reset(MsgBuf *buf)
{
    buf->len = 0;
    buf->pos = 0;
}

void msgbuf_free(MsgBuf *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->pos = 0;
}

void msgbuf_ensure(MsgBuf *buf, size_t additional)
{
    if (buf->len + additional <= buf->cap)
        return;

    size_t new_cap = buf->cap * 2;
    while (new_cap < buf->len + additional)
        new_cap *= 2;

    buf->data = realloc(buf->data, new_cap);
    buf->cap = new_cap;
}

void msgbuf_put_byte(MsgBuf *buf, uint8_t val)
{
    msgbuf_ensure(buf, 1);
    buf->data[buf->len++] = val;
}

void msgbuf_put_int16(MsgBuf *buf, int16_t val)
{
    msgbuf_ensure(buf, 2);
    int16_t nval = htons(val);
    memcpy(buf->data + buf->len, &nval, 2);
    buf->len += 2;
}

void msgbuf_put_int32(MsgBuf *buf, int32_t val)
{
    msgbuf_ensure(buf, 4);
    int32_t nval = htonl(val);
    memcpy(buf->data + buf->len, &nval, 4);
    buf->len += 4;
}

void msgbuf_put_string(MsgBuf *buf, const char *str)
{
    size_t slen = strlen(str) + 1; /* include null terminator */
    msgbuf_ensure(buf, slen);
    memcpy(buf->data + buf->len, str, slen);
    buf->len += slen;
}

void msgbuf_put_bytes(MsgBuf *buf, const void *data, size_t len)
{
    msgbuf_ensure(buf, len);
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
}

uint8_t msgbuf_get_byte(MsgBuf *buf)
{
    return buf->data[buf->pos++];
}

int16_t msgbuf_get_int16(MsgBuf *buf)
{
    int16_t val;
    memcpy(&val, buf->data + buf->pos, 2);
    buf->pos += 2;
    return ntohs(val);
}

int32_t msgbuf_get_int32(MsgBuf *buf)
{
    int32_t val;
    memcpy(&val, buf->data + buf->pos, 4);
    buf->pos += 4;
    return ntohl(val);
}

char* msgbuf_get_string(MsgBuf *buf)
{
    char *str = (char *)(buf->data + buf->pos);
    buf->pos += strlen(str) + 1;
    return str;
}

/*
 * Helper: begin a message with type byte, reserve space for length.
 * Returns the offset where the length field starts.
 */
static size_t msg_begin(MsgBuf *buf, uint8_t type)
{
    msgbuf_put_byte(buf, type);
    size_t len_offset = buf->len;
    msgbuf_put_int32(buf, 0); /* placeholder for length */
    return len_offset;
}

/*
 * Helper: finalize message by writing the correct length.
 * Length includes itself (4 bytes) but not the type byte.
 */
static void msg_end(MsgBuf *buf, size_t len_offset)
{
    int32_t len = (int32_t)(buf->len - len_offset);
    int32_t nlen = htonl(len);
    memcpy(buf->data + len_offset, &nlen, 4);
}

void msg_write_auth_ok(MsgBuf *buf)
{
    size_t off = msg_begin(buf, MSG_AUTH_REQUEST);
    msgbuf_put_int32(buf, AUTH_OK);
    msg_end(buf, off);
}

void msg_write_param_status(MsgBuf *buf, const char *name, const char *value)
{
    size_t off = msg_begin(buf, MSG_PARAM_STATUS);
    msgbuf_put_string(buf, name);
    msgbuf_put_string(buf, value);
    msg_end(buf, off);
}

void msg_write_backend_key(MsgBuf *buf, int32_t pid, int32_t key)
{
    size_t off = msg_begin(buf, MSG_BACKEND_KEY);
    msgbuf_put_int32(buf, pid);
    msgbuf_put_int32(buf, key);
    msg_end(buf, off);
}

void msg_write_ready_for_query(MsgBuf *buf, char txn_status)
{
    size_t off = msg_begin(buf, MSG_READY_FOR_QUERY);
    msgbuf_put_byte(buf, (uint8_t)txn_status);
    msg_end(buf, off);
}

void msg_write_error(MsgBuf *buf, const char *severity, const char *code,
                     const char *message)
{
    size_t off = msg_begin(buf, MSG_ERROR_RESPONSE);
    /* Severity field */
    msgbuf_put_byte(buf, 'S');
    msgbuf_put_string(buf, severity);
    /* SQLSTATE code */
    msgbuf_put_byte(buf, 'C');
    msgbuf_put_string(buf, code);
    /* Message */
    msgbuf_put_byte(buf, 'M');
    msgbuf_put_string(buf, message);
    /* Terminator */
    msgbuf_put_byte(buf, '\0');
    msg_end(buf, off);
}

void msg_write_command_complete(MsgBuf *buf, const char *tag)
{
    size_t off = msg_begin(buf, MSG_CMD_COMPLETE);
    msgbuf_put_string(buf, tag);
    msg_end(buf, off);
}
