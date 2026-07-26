#include "../title_encoding.h"

#include <stdio.h>
#include <string.h>

static int expect_conversion(const char* description, const char* shift_jis, const char* expected_utf8) {
    char actual[128];

    if (mdx_title_to_utf8(shift_jis, actual, sizeof(actual)) != 0) {
        fprintf(stderr, "%s: conversion failed\n", description);
        return 1;
    }
    if (strcmp(actual, expected_utf8) != 0) {
        fprintf(stderr, "%s: expected \"%s\", got \"%s\"\n", description, expected_utf8, actual);
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    char too_small[3];

    failures += expect_conversion("ASCII", "Gradius", "Gradius");
    failures += expect_conversion("kanji", "\x93\xfa\x96\x7b\x8c\xea", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
    failures += expect_conversion("half-width katakana", "\xb6\xc0\xb6\xc5", "\xef\xbd\xb6\xef\xbe\x80\xef\xbd\xb6\xef\xbe\x85");
    if (mdx_title_to_utf8("\x93\xfa", too_small, sizeof(too_small)) == 0) {
        fprintf(stderr, "small destination: conversion unexpectedly succeeded\n");
        failures++;
    }

    return failures == 0 ? 0 : 1;
}
