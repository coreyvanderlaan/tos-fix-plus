# LZMA SDK 23.01 (7z decoder only)

The 7z decoding part of Igor Pavlov's LZMA SDK 23.01 (https://www.7-zip.org/sdk.html,
lzma2301.7z, folder `C`), unchanged. The LZMA SDK is in the public domain. Used by
`textures.cpp` to read TSFix texture packs (`TSFix_Res\inject\*.7z`).

(TSFix's own bundled copy mixes files from different SDK versions and crashed when decoding.)
