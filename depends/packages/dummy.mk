# dummy package to learn how recipes work
#
# Following command will build only this package:
# make -C ${PWD}/depends HOST=$(depends/config.guess) -j$(nproc --all) dummy
# add V=1 for verbose output
#
# Force rebuild ("unconditional" consider all targets as outdated):
# make -B -C ${PWD}/depends HOST=$(depends/config.guess) V=1 -j$(nproc --all) dummy
#
# To build an individual package (useful for debugging), following build targets are available:
#
# PKG, PKG_fetched, PKG_extracted
# PKG_preprocessed, PKG_configured, PKG_built
# PKG_staged, PKG_postprocessed, PKG_cached
# PKG_cached_checksum
#
# Example:
# make -B -C ${PWD}/depends HOST=$(depends/config.guess) V=1 dummy_fetched
#
# Output all targets and rules:
# make -C ${PWD}/depends HOST=$(depends/config.guess) V=1 -pRrq
#
package=dummy
$(package)_version=1.0.0
$(package)_download_path=https://github.com/DeckerSU/dummy-package/archive/refs/tags/
$(package)_download_file=v$($(package)_version).tar.gz
$(package)_file_name=$(package)-$($(package)_version).tar.gz
$(package)_sha256_hash=b56360df29a29205f36dfe888d320369b7c859ca205376a9ca25180a7c931796

# optional variables

# $(package)_build_subdir  # cd to this dir before running configure/build/stage commands.
# $(package)_download_file # The file-name of the upstream source if it differs from how it should be stored locally.
# $(package)_dependencies  # Names of any other packages that this one depends on.
# $(package)_patches       # Filenames of any patches needed to build the package
# $(package)_extra_sources # Any extra files that will be fetched via $(package)_fetch_cmds.

## Build Variables:

# most variables can be prefixed with the host, architecture, or both
#    Universal:     $(package)_cc=gcc
#    Linux only:    $(package)_linux_cc=gcc
#    x86_64 only:       $(package)_x86_64_cc = gcc
#    x86_64 linux only: $(package)_x86_64_linux_cc = gcc

#
# *_env variables are used to add environment variables to the respective commands

# Operations order:
#
# 1. Fetching dummy...
# 2. Extracting dummy...
# 3. Preprocessing dummy...
# 4. Configuring dummy...
# 5. Building dummy...
# 6. Staging dummy...
# 7. Postprocessing dummy...
# 8. Caching dummy...

# https://unix.stackexchange.com/questions/269077/tput-setaf-color-table-how-to-determine-color-codes
# https://stackoverflow.com/questions/4879592/whats-the-difference-between-and-in-makefile # := and = difference
# https://www.gnu.org/software/make/manual/html_node/Flavors.html#Flavors

green:=$(shell tput setaf 10)
red:=$(shell tput setaf 9)
yellow:=$(shell tput setaf 11)
grey:=$(shell tput setaf 8)
reset:=$(shell tput sgr0)
$(info $(green)[ Dummy ]$(reset) Let's build the package ...)

# Example of making tool(s) variables
clang_prog=$(build_prefix)/bin/clang
clangxx_prog=$(clang_prog)++
cctools_TOOLS=AR RANLIB STRIP NM LIBTOOL OTOOL INSTALL_NAME_TOOL
lc = $(subst A,a,$(subst B,b,$(subst C,c,$(subst D,d,$(subst E,e,$(subst F,f,$(subst G,g,$(subst H,h,$(subst I,i,$(subst J,j,$(subst K,k,$(subst L,l,$(subst M,m,$(subst N,n,$(subst O,o,$(subst P,p,$(subst Q,q,$(subst R,r,$(subst S,s,$(subst T,t,$(subst U,u,$(subst V,v,$(subst W,w,$(subst X,x,$(subst Y,y,$(subst Z,z,$1))))))))))))))))))))))))))
$(foreach TOOL,$(cctools_TOOLS),$(eval special_$(TOOL) = $$(build_prefix)/bin/$$(host)-$(call lc,$(TOOL))))
$(foreach TOOL,$(cctools_TOOLS),$(info $(green)[ Dummy ]$(reset) $$(special_$(TOOL))=$(special_$(TOOL))))

define $(package)_set_vars
    $(package)_conf_tool=./configure
    $(package)_config_opts=--common-conf-option
    $(package)_config_opts_linux=--just-a-linux-conf-option
    $(package)_config_opts_mingw32=--special-windows-conf-option
endef

define $(package)_preprocess_cmds
    echo "$(grey)--------------------------------------------$(reset)" && \
    echo "Current directory: $(PWD)" && \
    echo "Global variables: " && \
    echo "$(grey)build_prefix:${reset} $(build_prefix)" && \
    echo "Path variables: " && \
    echo "$(grey)$(package)_staging_dir:${reset} $($(package)_staging_dir)" && \
    echo "$(grey)$(package)_staging_prefix_dir:${reset} $($(package)_staging_prefix_dir)" && \
    echo "$(grey)$(package)_extract_dir:${reset} $($(package)_extract_dir)" && \
    echo "$(grey)$(package)_build_dir:${reset} $($(package)_build_dir)" && \
    echo "$(grey)$(package)_build_subdir:${reset} $($(package)_build_subdir)" && \
    echo "$(grey)$(package)_patch_dir:${reset} $($(package)_patch_dir)" && \
    echo "Build variables: " && \
    echo "$(grey)$(package)_cc         :${reset} $($(package)_cc)" && \
    echo "$(grey)$(package)_cxx        :${reset} $($(package)_cxx)" && \
    echo "$(grey)$(package)_objc       :${reset} $($(package)_objc)" && \
    echo "$(grey)$(package)_objcxx     :${reset} $($(package)_objcxx)" && \
    echo "$(grey)$(package)_ar         :${reset} $($(package)_ar)" && \
    echo "$(grey)$(package)_ranlib     :${reset} $($(package)_ranlib)" && \
    echo "$(grey)$(package)_libtool    :${reset} $($(package)_libtool)" && \
    echo "$(grey)$(package)_nm         :${reset} $($(package)_nm)" && \
    echo "$(grey)$(package)_cflags     :${reset} $($(package)_cflags)" && \
    echo "$(grey)$(package)_cxxflags   :${reset} $($(package)_cxxflags)" && \
    echo "$(grey)$(package)_ldflags    :${reset} $($(package)_ldflags)" && \
    echo "$(grey)$(package)_cppflags   :${reset} $($(package)_cppflags)" && \
    echo "$(grey)$(package)_config_env :${reset} $($(package)_config_env)" && \
    echo "$(grey)$(package)_build_env  :${reset} $($(package)_build_env)" && \
    echo "$(grey)$(package)_stage_env  :${reset} $($(package)_stage_env)" && \
    echo "$(grey)$(package)_build_opts :${reset} $($(package)_build_opts)" && \
    echo "$(grey)$(package)_config_opts:${reset} $($(package)_config_opts)" && \
    echo "$(grey)--------------------------------------------$(reset)"
endef

#
define $(package)_config_cmds
  echo "$(yellow)$($(package)_conf_tool) $($(package)_config_opts)$(reset)"
endef

define $(package)_build_cmds
  echo "$(yellow)$(MAKE)$(reset)"
endef

# Most autotools projects can be properly staged using:
#    $(MAKE) DESTDIR=$($(package)_staging_dir) install
#
# For manual staging, for example, if we just want to stage
# 2 files, we can do it with the following way by copying them
# to $($(package)_staging_prefix_dir)
#
define $(package)_stage_cmds
    cp LICENSE $($(package)_staging_prefix_dir) && \
    cp README.md $($(package)_staging_prefix_dir)
endef
