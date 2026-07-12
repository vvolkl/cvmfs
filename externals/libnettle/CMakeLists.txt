# CMake module for the nettle low-level crypto library.
include(FetchContent)
include(CheckTypeSize)
include(TestBigEndian)

set(NETTLE_VERSION "3.10.2")
set(NETTLE_URL "https://ftp.gnu.org/gnu/nettle/nettle-${NETTLE_VERSION}.tar.gz")
set(NETTLE_URL_MIRROR "https://ecsft.cern.ch/dist/cvmfs/build_externals/nettle-${NETTLE_VERSION}.tar.gz")
set(NETTLE_HASH "MD5=b28bcbf6f045ff007940a9401673600d")

# -> nettle <-
# A system nettle >= 3.10 is preferred when available (the OPT_IN "try system
# first" default, or BUILTIN_EXTERNALS_EXCLUDE=nettle); the >= 3.10 floor is
# required because cvmfs/crypto uses SHAKE128 (sha3_128_shake), added in 3.10.
# Otherwise nettle is built from the vendored tarball below.
#
# NOTE: nettle is an autotools project (./configure + make). Like zlib, sqlite3
# and sparsehash we deliberately do NOT run its own build; we compile the
# libnettle C sources ourselves into a single static libnettle.a with a
# generated config.h. Only the portable C implementation is built -- no
# CPU-specific assembler (the .asm optimisations that ./configure would select
# live in nettle's OPT_NETTLE_SOURCES and are omitted) and no public-key code
# (hogweed / GMP). cvmfs only uses the symmetric primitives (md5, sha1, sha2,
# ripemd160, aes/cbc and shake128), all of which are covered by this pure-C build.

setup_external_build_mode(nettle)

FetchContent_Declare(
  Nettle
  URL "${NETTLE_URL}" "${NETTLE_URL_MIRROR}"
  URL_HASH "${NETTLE_HASH}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  DOWNLOAD_DIR ${EXTERNALS_DOWNLOAD_LOCATION}
  INSTALL_DIR ${EXTERNALS_INSTALL_LOCATION}
  EXCLUDE_FROM_ALL
  # Prefer a system nettle >= 3.10 (resolved via cmake/Modules/FindNettle.cmake).
  # When none is found FetchContent falls back to downloading + building the
  # vendored sources; the tarball has no CMakeLists.txt, so MakeAvailable only
  # downloads and extracts -- we define the target ourselves in the else() below.
  FIND_PACKAGE_ARGS 3.10
)

FetchContent_MakeAvailable(Nettle)
unset(FETCHCONTENT_TRY_FIND_PACKAGE_MODE)

if (Nettle_FOUND)
  message(STATUS "Found system nettle ${Nettle_VERSION}: ${NETTLE_LIBRARY}")
  # FindNettle already exposes the Nettle::Nettle imported target and the
  # NETTLE_* result variables. Promote them under the Libnettle_* names the rest
  # of the tree consumes (this runs from the externals/ subdirectory).
  set(Libnettle_LIBRARIES    "${NETTLE_LIBRARY}"     CACHE INTERNAL "nettle library")
  set(Libnettle_INCLUDE_DIRS "${NETTLE_INCLUDE_DIR}" CACHE INTERNAL "nettle include dir")
else ()
  message(STATUS "Building nettle from vendored sources in ${nettle_SOURCE_DIR}")

  # --- generate config.h (replaces nettle's ./configure output) -------------
  check_type_size("long"   SIZEOF_LONG)
  check_type_size("size_t" SIZEOF_SIZE_T)
  test_big_endian(NETTLE_IS_BIG_ENDIAN)

  set(HAVE_ALLOCA         1)
  set(HAVE_ALLOCA_H       1)
  set(HAVE_BUILTIN_BSWAP64 1)
  set(HAVE_GCC_ATTRIBUTE  1)
  set(HAVE_INTTYPES_H     1)
  set(HAVE_MEMORY_H       1)
  set(HAVE_STDINT_H       1)
  set(HAVE_STDLIB_H       1)
  set(HAVE_STRERROR       1)
  set(HAVE_STRINGS_H      1)
  set(HAVE_STRING_H       1)
  set(HAVE_SYS_STAT_H     1)
  set(HAVE_SYS_TYPES_H    1)
  set(HAVE_UNISTD_H       1)
  set(STDC_HEADERS        1)
  set(TIME_WITH_SYS_TIME  1)
  set(WORDS_BIGENDIAN     ${NETTLE_IS_BIG_ENDIAN})

  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nettle-config.h.in
                 ${CMAKE_CURRENT_BINARY_DIR}/config.h)

  # --- generate version.h (from nettle's own version.h.in) ------------------
  set(MAJOR_VERSION 3)
  set(MINOR_VERSION 10)
  # libnettle (symmetric crypto) needs neither GMP nor mini-gmp; only the
  # public-key hogweed library would. NETTLE_USE_MINI_GMP therefore stays 0 and
  # GMP_NUMB_BITS is never referenced (the version.h block is guarded by it).
  set(NETTLE_USE_MINI_GMP 0)
  set(GMP_NUMB_BITS 0)
  configure_file(${nettle_SOURCE_DIR}/version.h.in
                 ${CMAKE_CURRENT_BINARY_DIR}/version.h)

  # --- the static library ----------------------------------------------------
  # This is nettle's Makefile.in `nettle_SOURCES` list, i.e. everything that goes
  # into libnettle.a in the portable (assembler-free) configuration.
  set(NETTLE_SOURCES
    aes-decrypt-internal.c  aes-decrypt-table.c  aes-decrypt.c  aes-encrypt-internal.c
    aes-encrypt-table.c  aes-encrypt.c  aes-invert-internal.c  aes-set-decrypt-key.c
    aes-set-encrypt-key.c  aes-set-key-internal.c  aes128-decrypt.c  aes128-encrypt.c
    aes128-meta.c  aes128-set-decrypt-key.c  aes128-set-encrypt-key.c  aes192-decrypt.c
    aes192-encrypt.c  aes192-meta.c  aes192-set-decrypt-key.c  aes192-set-encrypt-key.c
    aes256-decrypt.c  aes256-encrypt.c  aes256-meta.c  aes256-set-decrypt-key.c
    aes256-set-encrypt-key.c  arcfour.c  arctwo-meta.c  arctwo.c
    balloon-sha1.c  balloon-sha256.c  balloon-sha384.c  balloon-sha512.c
    balloon.c  base16-decode.c  base16-encode.c  base16-meta.c
    base64-decode.c  base64-encode.c  base64-meta.c  base64url-decode.c
    base64url-encode.c  base64url-meta.c  blowfish-bcrypt.c  blowfish.c
    buffer-init.c  buffer.c  camellia-absorb.c  camellia-crypt-internal.c
    camellia-invert-key.c  camellia-table.c  camellia128-crypt.c  camellia128-meta.c
    camellia128-set-decrypt-key.c  camellia128-set-encrypt-key.c  camellia192-meta.c  camellia256-crypt.c
    camellia256-meta.c  camellia256-set-decrypt-key.c  camellia256-set-encrypt-key.c  cast128-meta.c
    cast128.c  cbc-aes128-encrypt.c  cbc-aes192-encrypt.c  cbc-aes256-encrypt.c
    cbc.c  ccm-aes128.c  ccm-aes192.c  ccm-aes256.c
    ccm.c  cfb.c  chacha-core-internal.c  chacha-crypt.c
    chacha-poly1305-meta.c  chacha-poly1305.c  chacha-set-key.c  chacha-set-nonce.c
    cmac-aes128-meta.c  cmac-aes128.c  cmac-aes256-meta.c  cmac-aes256.c
    cmac-des3-meta.c  cmac-des3.c  cmac.c  cmac64.c
    cnd-memcpy.c  ctr.c  ctr16.c  des.c
    des3.c  drbg-ctr-aes256.c  eax-aes128-meta.c  eax-aes128.c
    eax.c  gcm-aes.c  gcm-aes128-meta.c  gcm-aes128.c
    gcm-aes192-meta.c  gcm-aes192.c  gcm-aes256-meta.c  gcm-aes256.c
    gcm-camellia128-meta.c  gcm-camellia128.c  gcm-camellia256-meta.c  gcm-camellia256.c
    gcm-sm4-meta.c  gcm-sm4.c  gcm.c  ghash-set-key.c
    ghash-update.c  gost28147.c  gosthash94-meta.c  gosthash94.c
    hkdf.c  hmac-gosthash94-meta.c  hmac-gosthash94.c  hmac-md5-meta.c
    hmac-md5.c  hmac-ripemd160-meta.c  hmac-ripemd160.c  hmac-sha1-meta.c
    hmac-sha1.c  hmac-sha224-meta.c  hmac-sha224.c  hmac-sha256-meta.c
    hmac-sha256.c  hmac-sha384-meta.c  hmac-sha384.c  hmac-sha512-meta.c
    hmac-sha512.c  hmac-sm3-meta.c  hmac-sm3.c  hmac-streebog-meta.c
    hmac-streebog.c  hmac.c  knuth-lfib.c  md2-meta.c
    md2.c  md4-meta.c  md4.c  md5-compat.c
    md5-meta.c  md5.c  memeql-sec.c  memxor.c
    memxor3.c  nettle-lookup-hash.c  nettle-meta-aeads.c  nettle-meta-armors.c
    nettle-meta-ciphers.c  nettle-meta-hashes.c  nettle-meta-macs.c  nist-keywrap.c
    ocb-aes128.c  ocb.c  pbkdf2-hmac-gosthash94.c  pbkdf2-hmac-sha1.c
    pbkdf2-hmac-sha256.c  pbkdf2-hmac-sha384.c  pbkdf2-hmac-sha512.c  pbkdf2.c
    poly1305-aes.c  poly1305-internal.c  poly1305-update.c  realloc.c
    ripemd160-compress.c  ripemd160-meta.c  ripemd160.c  salsa20-128-set-key.c
    salsa20-256-set-key.c  salsa20-core-internal.c  salsa20-crypt-internal.c  salsa20-crypt.c
    salsa20-set-key.c  salsa20-set-nonce.c  salsa20r12-crypt.c  serpent-decrypt.c
    serpent-encrypt.c  serpent-meta.c  serpent-set-key.c  sha1-compress.c
    sha1-meta.c  sha1.c  sha224-meta.c  sha256-compress-n.c
    sha256-meta.c  sha256.c  sha3-224-meta.c  sha3-224.c
    sha3-256-meta.c  sha3-256.c  sha3-384-meta.c  sha3-384.c
    sha3-512-meta.c  sha3-512.c  sha3-permute.c  sha3-shake.c
    sha3.c  sha384-meta.c  sha512-224-meta.c  sha512-256-meta.c
    sha512-compress.c  sha512-meta.c  sha512.c  shake128.c
    shake256.c  siv-cmac-aes128.c  siv-cmac-aes256.c  siv-cmac.c
    siv-gcm-aes128.c  siv-gcm-aes256.c  siv-gcm.c  siv-ghash-set-key.c
    siv-ghash-update.c  sm3-meta.c  sm3.c  sm4-meta.c
    sm4.c  streebog-meta.c  streebog.c  twofish-meta.c
    twofish.c  umac-l2.c  umac-l3.c  umac-nh-n.c
    umac-nh.c  umac-poly128.c  umac-poly64.c  umac-set-key.c
    umac128.c  umac32.c  umac64.c  umac96.c
    version.c  write-be32.c  write-le32.c  write-le64.c
    xts-aes128.c  xts-aes256.c  xts.c  yarrow256.c
    yarrow_key_event.c
  )
  list(TRANSFORM NETTLE_SOURCES PREPEND "${nettle_SOURCE_DIR}/")

  add_library(nettle STATIC EXCLUDE_FROM_ALL ${NETTLE_SOURCES})
  target_compile_definitions(nettle PRIVATE HAVE_CONFIG_H)
  # nettle's sources include "config.h"/"version.h" (generated, in the binary
  # dir) and their flat sibling headers (in the source dir).
  target_include_directories(nettle PRIVATE
    ${nettle_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
  )
  set_target_properties(nettle PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # --- public headers for consumers -----------------------------------------
  # cvmfs includes nettle headers as <nettle/md5.h> etc. nettle ships them flat
  # in the source root, so reproduce the installed layout under include/nettle/.
  # This is nettle's Makefile.in `HEADERS` list plus the generated version.h.
  set(NETTLE_PUBLIC_HEADERS
    aes.h  arcfour.h  arctwo.h  asn1.h  balloon.h
    base16.h  base64.h  bignum.h  blowfish.h  buffer.h
    camellia.h  cast128.h  cbc.h  ccm.h  cfb.h
    chacha-poly1305.h  chacha.h  cmac.h  ctr.h  curve25519.h
    curve448.h  des.h  drbg-ctr.h  dsa-compat.h  dsa.h  eax.h
    ecc-curve.h  ecc.h  ecdsa.h  eddsa.h  gcm.h
    gostdsa.h  gosthash94.h  hkdf.h  hmac.h  knuth-lfib.h
    macros.h  md2.h  md4.h  md5-compat.h  md5.h
    memops.h  memxor.h  nettle-meta.h  nettle-types.h  nist-keywrap.h
    ocb.h  pbkdf2.h  pgp.h  pkcs1.h  poly1305.h
    pss-mgf1.h  pss.h  realloc.h  ripemd160.h  rsa.h
    salsa20.h  serpent.h  sexp.h  sha.h  sha1.h
    sha2.h  sha3.h  siv-cmac.h  siv-gcm.h  sm3.h
    sm4.h  streebog.h  twofish.h  umac.h  xts.h
    yarrow.h
  )
  set(NETTLE_PUBLIC_HEADER_PATHS "")
  foreach(_h ${NETTLE_PUBLIC_HEADERS})
    list(APPEND NETTLE_PUBLIC_HEADER_PATHS "${nettle_SOURCE_DIR}/${_h}")
  endforeach()
  file(COPY ${NETTLE_PUBLIC_HEADER_PATHS}
       DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include/nettle)
  configure_file(${nettle_SOURCE_DIR}/version.h.in
                 ${CMAKE_CURRENT_BINARY_DIR}/include/nettle/version.h)

  # Expose the library and its include directory to the rest of the tree, the
  # same way the other FetchContent externals do (consumed by cvmfs/crypto and
  # the micro benchmarks as ${Libnettle_LIBRARIES} / ${Libnettle_INCLUDE_DIRS}).
  add_library(Nettle::Nettle ALIAS nettle)
  set(Libnettle_LIBRARIES    nettle                                CACHE INTERNAL "nettle library")
  set(Libnettle_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/include" CACHE INTERNAL "nettle include dir")

  install(TARGETS nettle DESTINATION ${EXTERNALS_INSTALL_LOCATION} COMPONENT external_deps EXCLUDE_FROM_ALL)
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/nettle
          DESTINATION ${EXTERNALS_INSTALL_LOCATION}/include COMPONENT external_deps EXCLUDE_FROM_ALL)
  set(EXTERNALS_INSTALL_TARGETS ${EXTERNALS_INSTALL_TARGETS} "nettle" PARENT_SCOPE)
endif ()
