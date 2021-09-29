#ifndef RINGBUF__H__
#define RINGBUF__H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ringbuf;

struct ringbuf* create_ringbuf(size_t max);
void destroy_ringbuf(struct ringbuf** buf);
size_t ringbuf_size(const struct ringbuf* buf);
size_t ringbuf_capacity(const struct ringbuf* buf);
int ringbuf_empty(const struct ringbuf* buf);
int ringbuf_full(const struct ringbuf* buf);
/* writing to full buffer will overwrite tail */
void ringbuf_push(struct ringbuf* buf, uint32_t data);
/* Callers responsibility to check buffer not empty */
uint32_t ringbuf_pop(struct ringbuf* buf);

#ifdef __cplusplus
}
#endif

#endif /* RINGBUF__H__ */
