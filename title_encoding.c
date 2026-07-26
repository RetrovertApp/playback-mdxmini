#include "title_encoding.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <iconv.h>
#endif

int mdx_title_to_utf8(const char* shift_jis, char* utf8, size_t utf8_size) {
    if (shift_jis == NULL || utf8 == NULL || utf8_size == 0) {
        return -1;
    }
    utf8[0] = '\0';

#ifdef _WIN32
    int wide_size = MultiByteToWideChar(932, 0, shift_jis, -1, NULL, 0);
    if (wide_size == 0) {
        return -1;
    }

    wchar_t* wide = malloc((size_t)wide_size * sizeof(*wide));
    if (wide == NULL) {
        return -1;
    }
    if (MultiByteToWideChar(932, 0, shift_jis, -1, wide, wide_size) == 0) {
        free(wide);
        return -1;
    }

    int required_size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (required_size == 0 || (size_t)required_size > utf8_size) {
        free(wide);
        return -1;
    }

    int converted_size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, required_size, NULL, NULL);
    free(wide);
    return converted_size == 0 ? -1 : 0;
#else
    iconv_t converter = iconv_open("UTF-8", "CP932");
    if (converter == (iconv_t)-1) {
        return -1;
    }

    char* input = (char*)shift_jis;
    size_t input_left = strlen(shift_jis);
    char* output = utf8;
    size_t output_left = utf8_size - 1;

    while (input_left > 0) {
        if (iconv(converter, &input, &input_left, &output, &output_left) != (size_t)-1) {
            continue;
        }
        if (errno != EILSEQ && errno != EINVAL) {
            iconv_close(converter);
            utf8[0] = '\0';
            return -1;
        }

        // Keep the metadata valid UTF-8 even if a title contains a malformed byte.
        if (output_left < 3) {
            iconv_close(converter);
            utf8[0] = '\0';
            return -1;
        }
        *output++ = (char)0xef;
        *output++ = (char)0xbf;
        *output++ = (char)0xbd;
        output_left -= 3;
        input++;
        input_left--;
        iconv(converter, NULL, NULL, NULL, NULL);
    }

    *output = '\0';
    iconv_close(converter);
    return 0;
#endif
}
