# stb (vendored)

`stb_image.h` (v2.34) and `stb_image_write.h` from https://github.com/nothings/stb, public domain or
MIT (see the end of each file). The allsky vendor decodes camera JPEGs with `stb_image.h`, compiled
into one translation unit with `STB_IMAGE_STATIC`, so no decoder symbol is exported: the process
already holds libjpeg builds of two ABI versions (62 from Debian, 90 from elsewhere), and libjpeg
itself refused a mixed call ("Wrong JPEG library version: library is 90, caller expects 62").
`stb_image_write.h` is used by the tests only, to encode synthetic star fields.
