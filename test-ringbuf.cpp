#include <cstdint>
#include "ringbuf.h"

#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>

static int is_empty(size_t size) {return size == 0;}
static int is_full(size_t size, size_t cap) {return size == cap;}

static void check_ringbuf(const struct ringbuf* buf, size_t size, size_t cap)
{
	REQUIRE(buf);
	REQUIRE(ringbuf_size(buf) == size);
	REQUIRE(ringbuf_capacity(buf) == cap);
	REQUIRE(ringbuf_empty(buf) == is_empty(size));
	REQUIRE(ringbuf_full(buf) == is_full(size, cap));
}

TEST_CASE("Test empty") {
	struct ringbuf *buf = create_ringbuf(10);
	check_ringbuf(buf, 0, 10);
	destroy_ringbuf(&buf);

	destroy_ringbuf(&buf);
}

TEST_CASE("Test full") {
	struct ringbuf *buf = create_ringbuf(2);
	ringbuf_push(buf, 1);
	ringbuf_push(buf, 1);
	check_ringbuf(buf, 2, 2);

	destroy_ringbuf(&buf);
}

TEST_CASE("Test push") {
	struct ringbuf *buf = create_ringbuf(2);

	SECTION("Single") {
		ringbuf_push(buf, 1);
		check_ringbuf(buf, 1, 2);
	}

	SECTION("Dual") {
		ringbuf_push(buf, 1);
		ringbuf_push(buf, 2);
		check_ringbuf(buf, 2, 2);
	}

	SECTION("Triple") {
		ringbuf_push(buf, 1);
		ringbuf_push(buf, 2);
		ringbuf_push(buf, 3);
		check_ringbuf(buf, 2, 2);
	}

	destroy_ringbuf(&buf);
}

TEST_CASE("Test pop") {
	struct ringbuf *buf = create_ringbuf(2);

	SECTION("Single") {
		ringbuf_push(buf, 1);
		check_ringbuf(buf, 1, 2);

		const uint32_t v1 = ringbuf_pop(buf);
		REQUIRE(v1 == 1);
		check_ringbuf(buf, 0, 2);
	}

	SECTION("Dual") {
		ringbuf_push(buf, 1);
		ringbuf_push(buf, 2);
		check_ringbuf(buf, 2, 2);

		const uint32_t v1 = ringbuf_pop(buf);
		REQUIRE(v1 == 1);
		check_ringbuf(buf, 1, 2);

		const uint32_t v2 = ringbuf_pop(buf);
		REQUIRE(v2 == 2);
		check_ringbuf(buf, 0, 2);
	}

	SECTION("Overwrite") {
		ringbuf_push(buf, 1);
		ringbuf_push(buf, 2);
		ringbuf_push(buf, 3);
		check_ringbuf(buf, 2, 2);

		const uint32_t v2 = ringbuf_pop(buf);
		REQUIRE(v2 == 2);
		check_ringbuf(buf, 1, 2);

		const uint32_t v3 = ringbuf_pop(buf);
		REQUIRE(v3 == 3);
		check_ringbuf(buf, 0, 2);
	}

	destroy_ringbuf(&buf);
}

TEST_CASE("Moving average") {
	struct ringbuf *buf = create_ringbuf(10);
	for (size_t i = 0; i < ringbuf_capacity(buf); ++i) {
		ringbuf_push(buf, 0xAA);
	}
	check_ringbuf(buf, 10, 10);

	for (size_t i = 0; i < ringbuf_capacity(buf); ++i) {
		REQUIRE(ringbuf_pop(buf) == 0xAA);
		check_ringbuf(buf, 9, 10);
		ringbuf_push(buf, 0xFF);
		check_ringbuf(buf, 10, 10);
	}

	for (size_t i = 0; i < ringbuf_capacity(buf); ++i) {
		REQUIRE(ringbuf_pop(buf) == 0xFF);
		check_ringbuf(buf, ringbuf_capacity(buf) - (i + 1), 10);
	}

	destroy_ringbuf(&buf);
}
