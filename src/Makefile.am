DIST_SUBDIRS = secp256k1 univalue cryptoconditions

AM_LDFLAGS = $(PTHREAD_CFLAGS) $(LIBTOOL_LDFLAGS) $(SAN_LDFLAGS) $(HARDENED_LDFLAGS)
AM_CXXFLAGS = $(SAN_CXXFLAGS) $(HARDENED_CXXFLAGS) $(ERROR_CXXFLAGS)
AM_CPPFLAGS = $(HARDENED_CPPFLAGS)
EXTRA_LIBRARIES =

if EMBEDDED_LEVELDB
LEVELDB_CPPFLAGS += -I$(srcdir)/leveldb/include
LEVELDB_CPPFLAGS += -I$(srcdir)/leveldb/helpers/memenv
LIBLEVELDB += $(builddir)/leveldb/libleveldb.a
LIBMEMENV  += $(builddir)/leveldb/libmemenv.a

# NOTE: This dependency is not strictly necessary, but without it make may try to build both in parallel, which breaks the LevelDB build system in a race
$(LIBLEVELDB): $(LIBMEMENV)

$(LIBLEVELDB) $(LIBMEMENV):
	@echo "Building LevelDB ..." && $(MAKE) -C $(@D) $(@F) CXX="$(CXX)" \
	  CC="$(CC)" PLATFORM=$(TARGET_OS) AR="$(AR)" $(LEVELDB_TARGET_FLAGS) \
          OPT="$(AM_CXXFLAGS) $(PIE_FLAGS) $(CXXFLAGS) $(AM_CPPFLAGS) $(CPPFLAGS) -D__STDC_LIMIT_MACROS"
endif

BITCOIN_CONFIG_INCLUDES=-I$(builddir)/config
BITCOIN_INCLUDES=-I$(builddir) -I$(builddir)/obj $(BDB_CPPFLAGS) $(BOOST_CPPFLAGS) $(LEVELDB_CPPFLAGS) $(CRYPTO_CFLAGS) $(SSL_CFLAGS)

BITCOIN_INCLUDES += -I$(srcdir)/secp256k1/include
BITCOIN_INCLUDES += -I$(srcdir)/cc/includes
BITCOIN_INCLUDES += -I$(srcdir)/cryptoconditions/include
BITCOIN_INCLUDES += -I$(srcdir)/cryptoconditions/src
BITCOIN_INCLUDES += -I$(srcdir)/cryptoconditions/src/asn
BITCOIN_INCLUDES += -I$(srcdir)/snark
BITCOIN_INCLUDES += -I$(srcdir)/snark/libsnark
BITCOIN_INCLUDES += -I$(srcdir)/univalue/include

if TARGET_WINDOWS
LIBBITCOIN_SERVER=libbitcoin_server.a -lcurl
endif
if TARGET_DARWIN
LIBBITCOIN_SERVER=libbitcoin_server.a -lcurl
else
LIBBITCOIN_SERVER=libbitcoin_server.a -lcurl
endif

LIBBITCOIN_WALLET=libbitcoin_wallet.a
LIBBITCOIN_COMMON=libbitcoin_common.a
LIBBITCOIN_CLI=libbitcoin_cli.a
LIBBITCOIN_UTIL=libbitcoin_util.a
LIBBITCOIN_CRYPTO=crypto/libbitcoin_crypto.a
LIBBITCOINQT=qt/libmarmaraqt.a
LIBVERUS_CRYPTO=crypto/libverus_crypto.a
if ENABLE_SSE41
LIBBITCOIN_CRYPTO_SSE41 = crypto/libbitcoin_crypto_sse41.a
LIBBITCOIN_CRYPTO += $(LIBBITCOIN_CRYPTO_SSE41)
endif
if ENABLE_AVX2
LIBBITCOIN_CRYPTO_AVX2 = crypto/libbitcoin_crypto_avx2.a
LIBBITCOIN_CRYPTO += $(LIBBITCOIN_CRYPTO_AVX2)
endif
if ENABLE_SHANI
LIBBITCOIN_CRYPTO_SHANI = crypto/libbitcoin_crypto_shani.a
LIBBITCOIN_CRYPTO += $(LIBBITCOIN_CRYPTO_SHANI)
endif
LIBVERUS_PORTABLE_CRYPTO=crypto/libverus_portable_crypto.a
LIBSECP256K1=secp256k1/libsecp256k1.la
LIBCRYPTOCONDITIONS=cryptoconditions/libcryptoconditions_core.la
LIBSNARK=snark/libsnark.a
LIBUNIVALUE=univalue/libunivalue.la
LIBZCASH=libzcash.a -lcurl

$(LIBSECP256K1): $(wildcard secp256k1/src/*) $(wildcard secp256k1/include/*)
	$(AM_V_at)$(MAKE) $(AM_MAKEFLAGS) -C $(@D) $(@F)

LIBSNARK_CXXFLAGS = -fPIC -DBINARY_OUTPUT -DNO_PT_COMPRESSION=1 -fstack-protector-all
LIBSNARK_CONFIG_FLAGS = CURVE=ALT_BN128 NO_PROCPS=1 NO_DOCS=1 STATIC=1 NO_SUPERCOP=1 FEATUREFLAGS=-DMONTGOMERY_OUTPUT NO_COPY_DEPINST=1 NO_COMPILE_LIBGTEST=1
if HAVE_OPENMP
LIBSNARK_CONFIG_FLAGS += MULTICORE=1
endif

$(LIBSNARK): $(wildcard snark/src/*)
	$(AM_V_at) CXXFLAGS="$(LIBSNARK_CXXFLAGS)" $(MAKE) $(AM_MAKEFLAGS) -C snark/ DEPINST="$(LIBSNARK_DEPINST)" $(LIBSNARK_CONFIG_FLAGS) OPTFLAGS="-O2 -march=x86-64"

libsnark-tests: $(wildcard snark/src/*)
	$(AM_V_at) CXXFLAGS="$(LIBSNARK_CXXFLAGS)" $(MAKE) $(AM_MAKEFLAGS) -C snark/ check DEPINST="$(LIBSNARK_DEPINST)" $(LIBSNARK_CONFIG_FLAGS) OPTFLAGS="-O2 -march=x86-64"

$(LIBUNIVALUE): $(wildcard univalue/lib/*)
	$(AM_V_at)$(MAKE) $(AM_MAKEFLAGS) -C univalue/

$(LIBCRYPTOCONDITIONS): $(wildcard cryptoconditions/src/*) $(wildcard cryptoconditions/include/*)
	$(AM_V_at)$(MAKE) $(AM_MAKEFLAGS) -C $(@D) $(@F)

# Make is not made aware of per-object dependencies to avoid limiting building parallelization
# But to build the less dependent modules first, we manually select their order here:
EXTRA_LIBRARIES = \
  $(LIBBITCOIN_CRYPTO) \
  $(LIBVERUS_CRYPTO) \
  $(LIBVERUS_PORTABLE_CRYPTO) \
  $(LIBBITCOIN_UTIL) \
  $(LIBBITCOIN_COMMON) \
  $(LIBBITCOIN_SERVER) \
  $(LIBBITCOIN_CLI) \
  $(LIBZCASH)
if ENABLE_WALLET
BITCOIN_INCLUDES += $(BDB_CPPFLAGS)
EXTRA_LIBRARIES += $(LIBBITCOIN_WALLET)
endif
if ENABLE_ZMQ
EXTRA_LIBRARIES += $(LIBBITCOIN_ZMQ)
endif

if BUILD_KOMODO_LIBS
lib_LTLIBRARIES = libzcashconsensus.la
LIBZCASH_CONSENSUS=libzcashconsensus.la
else
LIBZCASH_CONSENSUS=
endif

bin_PROGRAMS =
noinst_PROGRAMS =
TESTS =

if BUILD_KOMODOD
#  temporary disable komodod build ... 
#  bin_PROGRAMS += komodod
endif

if BUILD_KOMODO_UTILS
  bin_PROGRAMS += komodo-cli
# bin_PROGRAMS += komodo-cli komodo-tx
endif

LIBZCASH_H = \
  zcash/IncrementalMerkleTree.hpp \
  zcash/NoteEncryption.hpp \
  zcash/Address.hpp \
  zcash/JoinSplit.hpp \
  zcash/Note.hpp \
  zcash/prf.h \
  zcash/Proof.hpp \
  zcash/util.h \
  zcash/Zcash.h \
  zcash/zip32.h

.PHONY: FORCE check-symbols check-security
# bitcoin core #
BITCOIN_CORE_H = \
  addressindex.h \
  spentindex.h \
  addrman.h \
  addrdb.h \
  alert.h \
  amount.h \
  amqp/amqpabstractnotifier.h \
  amqp/amqpconfig.h \
  amqp/amqpnotificationinterface.h \
  amqp/amqppublishnotifier.h \
  amqp/amqpsender.h \
  arith_uint256.h \
  asyncrpcoperation.h \
  asyncrpcqueue.h \
  base58.h \
  bech32.h \
  bloom.h \
  cc/eval.h \
  chain.h \
  chainparams.h \
  chainparamsbase.h \
  chainparamsseeds.h \
  checkpoints.h \
  checkqueue.h \
  clientversion.h \
  coincontrol.h \
  coins.h \
  compat.h \
  compat/byteswap.h \
  compat/endian.h \
  compat/sanity.h \
  compressor.h \
  consensus/consensus.h \
  consensus/merkle.h \
  consensus/params.h \
  consensus/upgrades.h \
  consensus/validation.h \
  core_io.h \
  core_memusage.h \
  crypto/haraka.h \
  crypto/haraka_portable.h \
  crypto/verus_hash.h \
  deprecation.h \
  fs.h \
  hash.h \
  httprpc.h \
  httpserver.h \
  init.h \
  key.h \
  key_io.h \
  keystore.h \
  dbwrapper.h \
  limitedmap.h \
  main.h \
  memusage.h \
  merkleblock.h \
  metrics.h \
  miner.h \
  mruset.h \
  net.h \
  netbase.h \
  notaries_staked.h \
  noui.h \
  paymentdisclosure.h \
  paymentdisclosuredb.h \
  policy/fees.h \
  pow.h \
  prevector.h \
  primitives/block.h \
  primitives/transaction.h \
  primitives/nonce.h \
  protocol.h \
  pubkey.h \
  random.h \
  reverselock.h \
  rpc/client.h \
  rpc/protocol.h \
  rpc/server.h \
  rpc/register.h \
  scheduler.h \
  script/interpreter.h \
  script/script.h \
  script/script_error.h \
  script/serverchecker.h \
  script/sign.h \
  script/standard.h \
  serialize.h \
  streams.h \
  support/allocators/secure.h \
  support/allocators/zeroafterfree.h \
  support/cleanse.h \
  support/events.h \
  support/pagelocker.h \
  sync.h \
  threadsafety.h \
  timedata.h \
  tinyformat.h \
  torcontrol.h \
  transaction_builder.h \
  txdb.h \
  txmempool.h \
  ui_interface.h \
  uint256.h \
  uint252.h \
  undo.h \
  util.h \
  utilmoneystr.h \
  utilstrencodings.h \
  utiltime.h \
  validationinterface.h \
  version.h \
  wallet/asyncrpcoperation_mergetoaddress.h \
  wallet/asyncrpcoperation_sendmany.h \
  wallet/asyncrpcoperation_shieldcoinbase.h \
  wallet/crypter.h \
  wallet/db.h \
  wallet/rpcwallet.h \
  wallet/wallet.h \
  wallet/wallet_ismine.h \
  wallet/walletdb.h \
  zmq/zmqabstractnotifier.h \
  zmq/zmqconfig.h\
  zmq/zmqnotificationinterface.h \
  zmq/zmqpublishnotifier.h


obj/build.h: FORCE
	@$(MKDIR_P) $(builddir)/obj
	@$(top_srcdir)/share/genbuild.sh $(abs_top_builddir)/src/obj/build.h \
	  $(abs_top_srcdir)
libbitcoin_util_a-clientversion.$(OBJEXT): obj/build.h

# server: shared between komodod and komodo-qt
libbitcoin_server_a_CPPFLAGS = $(BITCOIN_INCLUDES) $(MINIUPNPC_CPPFLAGS) $(EVENT_CFLAGS) $(EVENT_PTHREADS_CFLAGS)
# server: built-in cclib with custom cclib
libbitcoin_server_a_CPPFLAGS += -DBUILD_CUSTOMCC

libbitcoin_server_a_SOURCES = \
  sendalert.cpp \
  addrman.cpp \
  addrdb.cpp \
  alert.cpp \
  alertkeys.h \
  asyncrpcoperation.cpp \
  asyncrpcqueue.cpp \
  bloom.cpp \
  cc/eval.cpp \
  cc/import.cpp \
  cc/importgateway.cpp \
  cc/CCassetsCore.cpp \
  cc/old/CCassetsCore_v0.cpp \
  cc/CCassetstx.cpp \
  cc/old/CCassetstx_v0.cpp \
  cc/CCcustom.cpp \
  cc/CCtx.cpp \
  cc/CCutils.cpp \
  cc/CCvalidation.cpp \
  cc/CCtokens.cpp \
  cc/old/CCtokens_v0.cpp \
  cc/assets.cpp \
  cc/old/assets_v0.cpp \
  cc/faucet.cpp \
  cc/rewards.cpp \
  cc/dice.cpp \
  cc/lotto.cpp \
  cc/fsm.cpp \
  cc/heir.cpp \
  cc/oracles.cpp \
  cc/prices.cpp \
  cc/pegs.cpp \
  cc/marmara.cpp \
  cc/marmara_h0.cpp \
  cc/payments.cpp \
  cc/gateways.cpp \
  cc/channels.cpp \
  cc/auction.cpp \
  cc/betprotocol.cpp \
  cc/pricesfeed.cpp \
  cc/priceslibs/cjsonpointer.cpp \
  cc/kogs.cpp \
  chain.cpp \
  checkpoints.cpp \
  fs.cpp \
  crosschain.cpp \
  crosschain_authority.cpp \
  crypto/haraka.h \
  crypto/haraka_portable.h \
  crypto/verus_hash.h \
  crypto/verus_hash.cpp \
  deprecation.cpp \
  httprpc.cpp \
  httpserver.cpp \
  init.cpp \
  dbwrapper.cpp \
  main.cpp \
  merkleblock.cpp \
  metrics.cpp \
  miner.cpp \
  net.cpp \
  notaries_staked.cpp \
  noui.cpp \
  notarisationdb.cpp \
  paymentdisclosure.cpp \
  paymentdisclosuredb.cpp \
  policy/fees.cpp \
  pow.cpp \
  rest.cpp \
  rpc/blockchain.cpp \
  rpc/crosschain.cpp \
  rpc/mining.cpp \
  rpc/misc.cpp \
  rpc/net.cpp \
  rpc/rawtransaction.cpp \
  rpc/server.cpp \
  rpc/tokensrpc.cpp \
  rpc/pricesrpc.cpp \
  rpc/marmararpc.cpp \
  script/serverchecker.cpp \
  script/sigcache.cpp \
  timedata.cpp \
  torcontrol.cpp \
  txdb.cpp \
  txmempool.cpp \
  validationinterface.cpp \
  cc/cclib.cpp \
  $(BITCOIN_CORE_H) \
  $(LIBZCASH_H)

if ENABLE_ZMQ
LIBBITCOIN_ZMQ=libbitcoin_zmq.a

libbitcoin_zmq_a_CPPFLAGS = $(BITCOIN_INCLUDES) $(ZMQ_CFLAGS)
libbitcoin_zmq_a_SOURCES = \
	zmq/zmqabstractnotifier.cpp \
	zmq/zmqnotificationinterface.cpp \
	zmq/zmqpublishnotifier.cpp
endif


# wallet: shared between bitcoind and bitcoin-qt, but only linked
libbitcoin_wallet_a_CPPFLAGS = $(BITCOIN_INCLUDES)
libbitcoin_wallet_a_SOURCES = \
  utiltest.cpp \
  utiltest.h \
  zcbenchmarks.cpp \
  zcbenchmarks.h \
  wallet/asyncrpcoperation_mergetoaddress.cpp \
  wallet/asyncrpcoperation_sendmany.cpp \
  wallet/asyncrpcoperation_shieldcoinbase.cpp \
  wallet/crypter.cpp \
  wallet/db.cpp \
  paymentdisclosure.cpp \
  paymentdisclosuredb.cpp \
  transaction_builder.cpp \
  wallet/rpcdisclosure.cpp \
  wallet/rpcdump.cpp \
  cc/CCtx.cpp \
  wallet/rpcwallet.cpp \
  wallet/wallet.cpp \
  wallet/wallet_fees.cpp \
  wallet/wallet_ismine.cpp \
  wallet/walletdb.cpp \
  zcash/zip32.cpp \
  $(BITCOIN_CORE_H) \
  $(LIBZCASH_H)

# crypto primitives library
crypto_libbitcoin_crypto_a_CPPFLAGS = $(BITCOIN_CONFIG_INCLUDES)
crypto_libbitcoin_crypto_a_SOURCES = \
  crypto/common.h \
  crypto/equihash.cpp \
  crypto/equihash.h \
  crypto/equihash.tcc \
  crypto/hmac_sha256.cpp \
  crypto/hmac_sha256.h \
  crypto/hmac_sha512.cpp \
  crypto/hmac_sha512.h \
  crypto/ripemd160.cpp \
  crypto/ripemd160.h \
  crypto/sha1.cpp \
  crypto/sha1.h \
  crypto/sha256.cpp \
  crypto/sha256.h \
  crypto/sha512.cpp \
  crypto/sha512.h \
  crypto/haraka.h \
  crypto/haraka_portable.h \
  crypto/verus_hash.h \
  crypto/verus_hash.cpp

if USE_ASM
crypto_libbitcoin_crypto_a_SOURCES += crypto/sha256_sse4.cpp
endif

crypto_libbitcoin_crypto_sse41_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
crypto_libbitcoin_crypto_sse41_a_CPPFLAGS = $(AM_CPPFLAGS)
crypto_libbitcoin_crypto_sse41_a_CXXFLAGS += $(SSE41_CXXFLAGS)
crypto_libbitcoin_crypto_sse41_a_CPPFLAGS += -DENABLE_SSE41
crypto_libbitcoin_crypto_sse41_a_SOURCES = crypto/sha256_sse41.cpp

crypto_libbitcoin_crypto_avx2_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
crypto_libbitcoin_crypto_avx2_a_CPPFLAGS = $(AM_CPPFLAGS)
crypto_libbitcoin_crypto_avx2_a_CXXFLAGS += $(AVX2_CXXFLAGS)
crypto_libbitcoin_crypto_avx2_a_CPPFLAGS += -DENABLE_AVX2
crypto_libbitcoin_crypto_avx2_a_SOURCES = crypto/sha256_avx2.cpp

crypto_libbitcoin_crypto_shani_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
crypto_libbitcoin_crypto_shani_a_CPPFLAGS = $(AM_CPPFLAGS)
crypto_libbitcoin_crypto_shani_a_CXXFLAGS += $(SHANI_CXXFLAGS)
crypto_libbitcoin_crypto_shani_a_CPPFLAGS += -DENABLE_SHANI
crypto_libbitcoin_crypto_shani_a_SOURCES = crypto/sha256_shani.cpp

if ENABLE_MINING
EQUIHASH_TROMP_SOURCES = \
	pow/tromp/equi_miner.h \
	pow/tromp/equi.h \
	pow/tromp/osx_barrier.h

crypto_libbitcoin_crypto_a_CPPFLAGS += \
	-DEQUIHASH_TROMP_ATOMIC
crypto_libbitcoin_crypto_a_SOURCES += \
	${EQUIHASH_TROMP_SOURCES}
endif

# Verus hash specific library - optimized
crypto_libverus_crypto_a_CPPFLAGS = -O3 -march=x86-64 -msse4 -msse4.1 -msse4.2 -mssse3 -mavx -maes -g -funroll-loops -fomit-frame-pointer -fPIC $(AM_CPPFLAGS)
crypto_libverus_crypto_a_CXXFLAGS = -O3 -march=x86-64 -msse4 -msse4.1 -msse4.2 -mssse3 -mavx -maes -g -funroll-loops -fomit-frame-pointer -fPIC $(AM_CXXFLAGS)
crypto_libverus_crypto_a_SOURCES = \
  crypto/haraka.h \
  crypto/haraka.c

# Verus hash specific library - portable
crypto_libverus_portable_crypto_a_CPPFLAGS = -O3 -march=x86-64 -g -funroll-loops -fomit-frame-pointer -fPIC $(AM_CPPFLAGS)
crypto_libverus_portable_crypto_a_CXXFLAGS = -O3 -march=x86-64 -g -funroll-loops -fomit-frame-pointer -fPIC $(AM_CXXFLAGS)
crypto_libverus_portable_crypto_a_SOURCES = \
  crypto/haraka_portable.h \
  crypto/haraka_portable.c

# common: shared between zcashd and non-server tools
libbitcoin_common_a_CPPFLAGS = $(BITCOIN_INCLUDES)
libbitcoin_common_a_SOURCES = \
  amount.cpp \
  arith_uint256.cpp \
  base58.cpp \
  bech32.cpp \
  chainparams.cpp \
  coins.cpp \
  compressor.cpp \
  consensus/merkle.cpp \
  consensus/upgrades.cpp \
  core_read.cpp \
  core_write.cpp \
  crypto/haraka.h \
  crypto/haraka_portable.h \
  crypto/verus_hash.h \
  crypto/verus_hash.cpp \
  hash.cpp \
  importcoin.cpp \
  key.cpp \
  key_io.cpp \
  keystore.cpp \
  netbase.cpp \
  metrics.cpp \
  primitives/block.cpp \
  primitives/transaction.cpp \
  primitives/nonce.cpp \
  protocol.cpp \
  pubkey.cpp \
  scheduler.cpp \
  script/cc.cpp \
  script/interpreter.cpp \
  script/script.cpp \
  script/script_ext.cpp \
  script/script_error.cpp \
  script/sign.cpp \
  script/standard.cpp \
  transaction_builder.cpp \
  cc/CCtokenutils.cpp \
  cc/old/CCtokenutils_v0.cpp \
  cc/CCutilbits.cpp \
  gmp_i64.cpp \
  $(BITCOIN_CORE_H) \
  $(LIBZCASH_H)

# util: shared between all executables.
# This library *must* be included to make sure that the glibc
# backward-compatibility objects and their sanity checks are linked.
libbitcoin_util_a_CPPFLAGS = $(BITCOIN_INCLUDES)
libbitcoin_util_a_SOURCES = \
  support/pagelocker.cpp \
  chainparamsbase.cpp \
  clientversion.cpp \
  compat/glibc_sanity.cpp \
  compat/glibcxx_sanity.cpp \
  compat/strnlen.cpp \
  random.cpp \
  rpc/protocol.cpp \
  support/cleanse.cpp \
  sync.cpp \
  uint256.cpp \
  util.cpp \
  utilmoneystr.cpp \
  utilstrencodings.cpp \
  utiltime.cpp \
  $(BITCOIN_CORE_H) \
  $(LIBZCASH_H)

if GLIBC_BACK_COMPAT
libbitcoin_util_a_SOURCES += compat/glibc_compat.cpp
endif

# cli: zcash-cli
libbitcoin_cli_a_CPPFLAGS = $(BITCOIN_INCLUDES)
libbitcoin_cli_a_SOURCES = \
  rpc/client.cpp \
  $(BITCOIN_CORE_H) \
  $(LIBZCASH_H)

nodist_libbitcoin_util_a_SOURCES = $(srcdir)/obj/build.h
#

# bitcoind binary #
komodod_SOURCES = bitcoind.cpp
komodod_CPPFLAGS = $(BITCOIN_INCLUDES)
komodod_LDFLAGS = $(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS)

if TARGET_WINDOWS
komodod_SOURCES += bitcoind-res.rc
endif

komodod_LDADD = \
  $(LIBBITCOIN_SERVER) \
  $(LIBBITCOIN_COMMON) \
  $(LIBUNIVALUE) \
  $(LIBBITCOIN_UTIL) \
  $(LIBBITCOIN_CRYPTO) \
  $(LIBVERUS_CRYPTO) \
  $(LIBVERUS_PORTABLE_CRYPTO) \
  $(LIBZCASH) \
  $(LIBSNARK) \
  $(LIBLEVELDB) \
  $(LIBMEMENV) \
  $(LIBSECP256K1) \
  $(LIBCRYPTOCONDITIONS)

if ENABLE_ZMQ
komodod_LDADD += $(LIBBITCOIN_ZMQ) $(ZMQ_LIBS)
endif

if ENABLE_WALLET
komodod_LDADD += $(LIBBITCOIN_WALLET)
endif

komodod_LDADD += \
  $(BOOST_LIBS) \
  $(BDB_LIBS) \
  $(SSL_LIBS) \
  $(CRYPTO_LIBS) \
  $(MINIUPNPC_LIBS) \
  $(EVENT_PTHREADS_LIBS) \
  $(EVENT_LIBS) \
  $(LIBZCASH) \
  $(LIBBITCOIN_CRYPTO) \
  $(LIBVERUS_CRYPTO) \
  $(LIBVERUS_PORTABLE_CRYPTO) \
  $(LIBSNARK) \
  $(LIBZCASH_LIBS)

komodod_LDADD += $(LIBSECP256K1)

# bitcoin-cli binary #
komodo_cli_SOURCES = bitcoin-cli.cpp
komodo_cli_CPPFLAGS = $(BITCOIN_INCLUDES) $(EVENT_CFLAGS)
komodo_cli_LDFLAGS = $(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS)

if TARGET_WINDOWS
komodo_cli_SOURCES += bitcoin-cli-res.rc
endif

komodo_cli_LDADD = \
  $(LIBBITCOIN_CLI) \
  $(LIBUNIVALUE) \
  $(LIBBITCOIN_UTIL) \
  $(BOOST_LIBS) \
  $(SSL_LIBS) \
  $(CRYPTO_LIBS) \
  $(EVENT_LIBS) \
  $(LIBZCASH) \
  $(LIBSNARK) \
  $(LIBVERUS_CRYPTO) \
  $(LIBVERUS_PORTABLE_CRYPTO) \
  $(LIBBITCOIN_CRYPTO) \
  $(LIBZCASH_LIBS)
#

# zcash-tx binary #
komodo_tx_SOURCES = komodo-tx.cpp
komodo_tx_CPPFLAGS = $(BITCOIN_INCLUDES)
komodo_tx_LDFLAGS = $(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS)

if TARGET_WINDOWS
komodo_tx_SOURCES += bitcoin-tx-res.rc
endif

# FIXME: Is libzcash needed for zcash_tx?
komodo_tx_LDADD = \
  $(LIBUNIVALUE) \
  $(LIBBITCOIN_COMMON) \
  $(LIBBITCOIN_UTIL) \
  $(LIBSECP256K1) \
  $(LIBZCASH) \
  $(LIBSNARK) \
  $(LIBBITCOIN_CRYPTO) \
  $(LIBVERUS_CRYPTO) \
  $(LIBVERUS_PORTABLE_CRYPTO) \
  $(LIBZCASH_LIBS) \
  $(LIBCRYPTOCONDITIONS)

komodo_tx_LDADD += $(BOOST_LIBS) $(CRYPTO_LIBS)
#

# zcash protocol primitives #
libzcash_a_SOURCES = \
	zcash/IncrementalMerkleTree.cpp \
	zcash/NoteEncryption.cpp \
	zcash/Address.cpp \
	zcash/JoinSplit.cpp \
	zcash/Proof.cpp \
	zcash/Note.cpp \
	zcash/prf.cpp \
	zcash/util.cpp \
  zcash/zip32.cpp \
	zcash/circuit/commitment.tcc \
	zcash/circuit/gadget.tcc \
	zcash/circuit/merkle.tcc \
	zcash/circuit/note.tcc \
	zcash/circuit/prfs.tcc \
	zcash/circuit/utils.tcc

libzcash_a_CPPFLAGS = -DMULTICORE -fopenmp -fPIC -DBINARY_OUTPUT -DCURVE_ALT_BN128 -DBOOST_SPIRIT_THREADSAFE -DHAVE_BUILD_INFO -D__STDC_FORMAT_MACROS $(HARDENED_CPPFLAGS) -pipe -O1 -g -Wstack-protector -fstack-protector-all -fPIE -fvisibility=hidden -DSTATIC $(BITCOIN_INCLUDES)

libzcash_a_CXXFLAGS = $(HARDENED_CXXFLAGS) -fwrapv -fno-strict-aliasing

#libzcash_a_LDFLAGS = $(HARDENED_LDFLAGS)

libzcash_a_CPPFLAGS += -DMONTGOMERY_OUTPUT

# zcashconsensus library #
if BUILD_KOMODO_LIBS
include_HEADERS = 
libzcashconsensus_la_SOURCES = \
  crypto/equihash.cpp \
  crypto/hmac_sha512.cpp \
  crypto/ripemd160.cpp \
  crypto/sha1.cpp \
  crypto/sha256.cpp \
  crypto/sha512.cpp \
  hash.cpp \
  primitives/transaction.cpp \
  primitives/nonce.cpp \
  pubkey.cpp \
  script/zcashconsensus.cpp \
  script/interpreter.cpp \
  script/script.cpp \
  uint256.cpp \
  utilstrencodings.cpp

if GLIBC_BACK_COMPAT
  libzcashconsensus_la_SOURCES += compat/glibc_compat.cpp
endif

libzcashconsensus_la_LDFLAGS = -no-undefined $(RELDFLAGS)
libzcashconsensus_la_LIBADD = $(LIBSECP256K1)
libzcashconsensus_la_LIBADD = $(CRYPTO_LIBS)
libzcashconsensus_la_CPPFLAGS = $(CRYPTO_CFLAGS) -I$(builddir)/obj -I$(srcdir)/secp256k1/include -I$(srcdir)/cryptoconditions/include -DBUILD_BITCOIN_INTERNAL

endif
#

CLEANFILES = leveldb/libleveldb.a leveldb/libmemenv.a *.gcda *.gcno */*.gcno wallet/*/*.gcno

DISTCLEANFILES = obj/build.h

EXTRA_DIST = leveldb snark

clean-local:
	-$(MAKE) -C leveldb clean
	-$(MAKE) -C secp256k1 clean
	-$(MAKE) -C snark clean
	-$(MAKE) -C univalue clean
	-$(MAKE) -C cryptoconditions clean
	rm -f leveldb/*/*.gcno leveldb/helpers/memenv/*.gcno
	-rm -f config.h

.rc.o:
	@test -f $(WINDRES)
	$(AM_V_GEN) $(WINDRES) -DWINDRES_PREPROC -i $< -o $@

.mm.o:
	$(AM_V_CXX) $(OBJCXX) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) \
	  $(CPPFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS)  -c -o $@ $<

check-symbols: $(bin_PROGRAMS)
if GLIBC_BACK_COMPAT
	@echo "Checking glibc back compat of [$(bin_PROGRAMS)]..."
	$(AM_V_at) READELF=$(READELF) CPPFILT=$(CPPFILT) $(top_srcdir)/contrib/devtools/symbol-check.py < $(bin_PROGRAMS)
endif

check-security: $(bin_PROGRAMS)
if HARDEN
	@echo "Checking binary security of [$(bin_PROGRAMS)]..."
	$(AM_V_at) READELF=$(READELF) OBJDUMP=$(OBJDUMP) $(top_srcdir)/contrib/devtools/security-check.py < $(bin_PROGRAMS)
endif

if ENABLE_BIP70
%.pb.cc %.pb.h: %.proto
	@test -f $(PROTOC)
	$(AM_V_GEN) $(PROTOC) --cpp_out=$(@D) --proto_path=$(abspath $(<D) $<)
endif

if ENABLE_TESTS
include Makefile.ktest.include
#include Makefile.test.include
#include Makefile.gtest.include
endif

#include Makefile.zcash.include

#if ENABLE_QT
include Makefile.qt.include
#endif
