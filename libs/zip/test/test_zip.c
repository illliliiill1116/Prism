/* Tests against test/fixtures/sample.zip (made with Python's zipfile):
 * stored and deflated entries, and a nested path. */
#include "zip_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FIXTURES_DIR
#define FIXTURES_DIR "test/fixtures"
#endif

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        failures++; \
    } \
} while (0)

static void check_entry(const zip_archive_t *archive, const char *path,
                        uint16_t expect_method, const char *expect_content, size_t expect_len) {
    const zip_entry_t *e = zip_findz(archive, path);
    CHECK(e != NULL, "entry found");
    if (!e) return;
    CHECK(e->method == expect_method, "entry has expected compression method");

    uint8_t *buf;
    size_t len;
    zip_result_t r = zip_extract(archive, e, &buf, &len);
    CHECK(r == ZIP_OK, "entry extracts without error");
    if (r != ZIP_OK) return;
    CHECK(len == expect_len && memcmp(buf, expect_content, len) == 0, "extracted content matches exactly");
    free(buf);
}

int main(void) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/sample.zip", FIXTURES_DIR);

    zip_archive_t archive;
    zip_result_t r = zip_open(path, &archive);
    CHECK(r == ZIP_OK, "sample.zip opens");
    if (r != ZIP_OK) { printf("%d test(s) failed\n", failures + 1); return 1; }

    CHECK(archive.entry_count == 3, "archive has 3 entries");

    char hello_expected[23 * 20 + 1];
    hello_expected[0] = '\0';
    for (int i = 0; i < 20; i++) strcat(hello_expected, "Hello, deflate world! ");
    check_entry(&archive, "hello.txt", 8, hello_expected, strlen(hello_expected));

    check_entry(&archive, "stored.txt", 0, "stored, no compression", strlen("stored, no compression"));
    check_entry(&archive, "dir/nested.txt", 8, "nested entry content", strlen("nested entry content"));

    CHECK(zip_findz(&archive, "does/not/exist.txt") == NULL, "missing entry returns NULL");

    zip_close(&archive);

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
