package=zlib
$(package)_version=1.3
$(package)_download_path=https://github.com/madler/zlib/releases/download/v$($(package)_version)
$(package)_file_name=$(package)-$($(package)_version).tar.xz
$(package)_sha256_hash=8a9ba2898e1d0d774eca6ba5b4627a11e5588ba85c8851336eb38de4683050a7

define $(package)_set_vars
$(package)_config_opts= CC="$($(package)_cc)"
$(package)_config_opts+=CFLAGS="$($(package)_cflags) $($(package)_cppflags) -fPIC"
$(package)_config_opts+=RANLIB="$($(package)_ranlib)"
$(package)_config_opts+=AR="$($(package)_ar)"
$(package)_config_opts_darwin+=AR="$($(package)_libtool)"
$(package)_config_opts_darwin+=ARFLAGS="-o"
$(package)_config_opts_android+=CHOST=$(host)
endef

# zlib has its own custom configure script that takes in options like CC,
# CFLAGS, RANLIB, AR, and ARFLAGS from the environment rather than from
# command-line arguments.
define $(package)_config_cmds
  env $($(package)_config_opts) ./configure --static --prefix=$(host_prefix)
endef

define $(package)_build_cmds
  $(MAKE) libz.a
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

