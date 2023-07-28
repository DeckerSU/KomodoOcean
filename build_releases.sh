#!/usr/bin/env bash

delete_linux_depends=false
force_rebuild_containers=false

build_xenial=false
build_focal=false
build_windows=true
build_macos=false

# we should rebuild linux depends before build for different linux os
if [[ $build_xenial == true && $build_focal == true ]]; then
  delete_linux_depends=true
fi

download_and_check_macos_sdk() {
    url="https://bitcoincore.org/depends-sources/sdks/Xcode-12.1-12A7403-extracted-SDK-with-libcxx-headers.tar.gz"
    output_file="Xcode-12.1-12A7403-extracted-SDK-with-libcxx-headers.tar.gz"
    expected_checksum="be17f48fd0b08fb4dcd229f55a6ae48d9f781d210839b4ea313ef17dd12d6ea5"

    # Check if file exists
    if [[ -f "$output_file" ]]; then
        # Calculate checksum of the file
        actual_checksum=$(sha256sum "$output_file" 2>/dev/null | awk '{print $1}')
        if [[ -n $actual_checksum ]]; then
            # Compare checksums
            if [[ "$actual_checksum" == "$expected_checksum" ]]; then
                echo "File already exists and has the correct checksum. Skipping download."
                return
            fi
        fi
    fi

    echo "Downloading file..."
    # Download the file
    curl -L -o "$output_file" "$url"

    # Calculate checksum of the downloaded file
    actual_checksum=$(sha256sum "$output_file" | awk '{print $1}')

    # Compare checksums
    if [[ "$actual_checksum" != "$expected_checksum" ]]; then
        echo "ERROR: Downloaded file has an invalid checksum."
        exit 1
    fi

    echo "File downloaded successfully and has a valid checksum."
}

delete_artifacts() {
    local release_name=$1

    if [[ "$release_name" = "windows" ]]; then
    ext=".exe"
    else
    ext=""
    fi

    mkdir -p ${WORKSPACE}/releases/${release_name}

    binaries=(
    "src/komodod"
    "src/wallet-utility"
    "src/komodo-tx"
    "src/komodo-cli"
    "src/komodo-test"
    "src/qt/komodo-qt"
    )

    for binary in "${binaries[@]}"
    do
    rm -f "${WORKSPACE}/${binary}${ext}" || false
    done

    echo "Deleting artifacts from ${WORKSPACE} ..."
    # delete possible artifacts from previous build(s)
    find ${WORKSPACE}/src \( -name "*.a" -o -name "*.la" -o -name "*.o" -o -name "*.lo" -o -name "*.Plo" -o -name "*.Po" -o -name "*.lai" -o -name "*.dirstamp" \) -delete # -print 
    find ${WORKSPACE}/src \( -name "*.a" -o -name "*.la" -o -name "*.o" -o -name "*.lo" -o -name "*.Plo" -o -name "*.Po" -o -name "*.lai" -o -name "*.dirstamp" \) -path "*/.*" -delete # -print 
}

copy_release() {
    local release_name=$1

    if [[ "$release_name" = "windows" ]]; then
    ext=".exe"
    else
    ext=""
    fi

    mkdir -p ${WORKSPACE}/releases/${release_name}

    binaries=(
    "src/komodod"
    "src/wallet-utility"
    "src/komodo-tx"
    "src/komodo-cli"
    "src/qt/komodo-qt"
    )

    for binary in "${binaries[@]}"
    do
    strip "${WORKSPACE}/${binary}${ext}" || false
    cp -f "${WORKSPACE}/${binary}${ext}" "${WORKSPACE}/releases/${release_name}/"
    done

    case $release_name in
        xenial)
            echo "Performing actions for Xenial..."
            mv "${WORKSPACE}/releases/${release_name}/komodo-qt" "${WORKSPACE}/releases/${release_name}/komodo-qt-linux"
            ;;
        focal)
            echo "Performing actions for Focal..."
            mv "${WORKSPACE}/releases/${release_name}/komodo-qt" "${WORKSPACE}/releases/${release_name}/komodo-qt-linux"
            ;;
        *)
            echo "Unknown release name: $release_name"
            ;;
    esac
}

# https://stackoverflow.com/questions/4774054/reliable-way-for-a-bash-script-to-get-the-full-path-to-itself
SCRIPTPATH="$( cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P )"
pushd ${SCRIPTPATH}
WORKSPACE=$(pwd)

echo "Workspace directory: ${WORKSPACE}"

# Check if awk command exists
command -v awk >/dev/null 2>&1 || { echo >&2 "ERROR: awk command not found."; exit 1; }
# Check if sha256sum command exists
command -v sha256sum >/dev/null 2>&1 || { echo >&2 "ERROR: sha256sum command not found."; exit 1; }

### xenial
if [[ "${build_xenial}" = "true" ]]; then

    # delete old depends binaries (from previous linux version, bcz it's x86_64-unknown-linux-gnu also)
    if [[ "${delete_linux_depends}" = true ]]; then
        rm -rf ${WORKSPACE}/depends/built/x86_64-unknown-linux-gnu
        rm -rf ${WORKSPACE}/depends/x86_64-unknown-linux-gnu
    fi
    # delete possible artifacts from previous build(s)
    delete_artifacts xenial

    if [[ "${force_rebuild_containers}" == "true" || "$(docker images --format '{{.Repository}}' | grep -c ocean_xenial_builder)" -eq 0 ]]; then
        echo "Container 'ocean_xenial_builder' rebuilding ..."
        docker build -f Dockerfile.xenial.ci --build-arg BUILDER_NAME=$USER --build-arg BUILDER_UID=$(id -u) --build-arg BUILDER_GID=$(id -g) -t ocean_xenial_builder .
    fi

    docker run -u $(id -u ${USER}):$(id -g ${USER}) -v $PWD:$PWD -w $PWD -e HOME=/root ocean_xenial_builder /bin/bash -c 'zcutil/build.sh -j'$(expr $(nproc) - 1)
    copy_release xenial
fi

### focal
if [[ "${build_focal}" = "true" ]]; then

    # delete old depends binaries (from previous linux version, bcz it's x86_64-unknown-linux-gnu also)
    if [[ "${delete_linux_depends}" = true ]]; then
        rm -rf ${WORKSPACE}/depends/built/x86_64-unknown-linux-gnu
        rm -rf ${WORKSPACE}/depends/x86_64-unknown-linux-gnu
    fi
    # delete possible artifacts from previous build(s)
    delete_artifacts focal

    if [[ "${force_rebuild_containers}" == "true" || "$(docker images --format '{{.Repository}}' | grep -c ocean_focal_builder)" -eq 0 ]]; then
        echo "Container 'ocean_focal_builder' rebuilding ..."
        docker build -f Dockerfile.focal.ci --build-arg BUILDER_NAME=$USER --build-arg BUILDER_UID=$(id -u) --build-arg BUILDER_GID=$(id -g) -t ocean_focal_builder .
    fi

    docker run -u $(id -u ${USER}):$(id -g ${USER}) -v $PWD:$PWD -w $PWD -e HOME=/root ocean_focal_builder /bin/bash -c 'zcutil/build.sh -j'$(expr $(nproc) - 1)
    copy_release focal
fi

### windows
if [[ "${build_windows}" = "true" ]]; then
    delete_artifacts windows

    if [[ "${force_rebuild_containers}" == "true" || "$(docker images --format '{{.Repository}}' | grep -c ocean_focal_builder)" -eq 0 ]]; then
        echo "Container 'ocean_focal_builder' rebuilding ..."
        docker build -f Dockerfile.focal.ci --build-arg BUILDER_NAME=$USER --build-arg BUILDER_UID=$(id -u) --build-arg BUILDER_GID=$(id -g) -t ocean_focal_builder .
    fi

    docker run -u $(id -u ${USER}):$(id -g ${USER}) -v $PWD:$PWD -w $PWD -e HOME=/root ocean_focal_builder /bin/bash -c 'zcutil/build-win.sh -j$(nproc --all)'
    copy_release windows
fi

### macos
if [[ "${build_macos}" = "true" ]]; then
    echo "Building MacOs ... "
fi

popd