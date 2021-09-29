#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include "ringbuf.h"

struct ringbuf {
	size_t size;
	size_t max;
	size_t head;
	size_t tail;
	uint32_t *data;
};

struct ringbuf* create_ringbuf(size_t max)
{
	struct ringbuf *buf = malloc(sizeof(struct ringbuf) + (sizeof(uint32_t) * max));
	if (buf) {
		buf->size = 0;
		buf->max = max;
		buf->head = 0;
		buf->tail = 0;
		buf->data = (uint32_t*) ((uint8_t*) buf + sizeof(struct ringbuf));
	}
	return buf;
}

void destroy_ringbuf(struct ringbuf** buf)
{
	if (*buf) {
		free(*buf);
		*buf = NULL;
	}
}

size_t ringbuf_size(const struct ringbuf* buf)
{
	return buf->size;
}

size_t ringbuf_capacity(const struct ringbuf* buf)
{
	return buf->max;
}

int ringbuf_empty(const struct ringbuf* buf)
{
	return (!buf->size);
}

int ringbuf_full(const struct ringbuf* buf)
{
	return buf->size == buf->max;
}

void ringbuf_push(struct ringbuf* buf, uint32_t data)
{
	buf->data[buf->head] = data;
	buf->head = (buf->head + 1) % buf->max;
	if (buf->size < buf->max) {
		buf->size++;
	}
	else {
		buf->tail = (buf->tail + 1) % buf->max;
	}
}

uint32_t ringbuf_pop(struct ringbuf* buf)
{
	uint32_t data = buf->data[buf->tail];
	buf->tail = (buf->tail + 1) % buf->max;
	buf->size--;
	return data;
}
