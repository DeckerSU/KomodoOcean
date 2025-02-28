#!/usr/bin/env bash

groupadd --gid ${BUILDER_GID} --force ${BUILDER_NAME}
adduser --disabled-password --gecos '' --no-create-home $BUILDER_NAME --uid ${BUILDER_UID} --gid ${BUILDER_GID}
adduser $BUILDER_NAME sudo
echo "$BUILDER_NAME ALL=(ALL:ALL) NOPASSWD: ALL" | tee /etc/sudoers.d/$BUILDER_NAME

# there may be a better way to continue building as a user builder with the same UID and GID as the host runner
su -m $BUILDER_NAME << 'EOF'
echo "User: $(whoami)"
WORKSPACE=$(pwd)
echo "Workspace directory: ${WORKSPACE}"

delete_linux_depends=false

build_focal=true
build_windows=true
build_macos=true

download_and_check_macos_sdk() {
    url="https://download.decker.im/depends-sources/Xcode-13.2.1-13C100-extracted-SDK-with-libcxx-headers.tar.gz"
    output_file="Xcode-13.2.1-13C100-extracted-SDK-with-libcxx-headers.tar.gz"
    expected_checksum="3d200832a74bc7401160043c92f68b11fcedba2d01353b5ab204f9e6a65653d1"

    # Check if file exists
    if [[ -f "$output_file" ]]; then
        # Calculate checksum of the file
        actual_checksum=$(sha256sum "$output_file" 2>/dev/null | awk '{print $1}')
        if [[ -n $actual_checksum ]]; then
            # Compare checksums
            if [[ "$actual_checksum" == "$expected_checksum" ]]; then
                echo "MacOS SDK already exists and has the correct checksum. Skipping download."
                return
            fi
        fi
    fi

    echo "Downloading MacOS SDK ..."
    # Download the file
    curl -L -o "$output_file" "$url"

    # Calculate checksum of the downloaded file
    actual_checksum=$(sha256sum "$output_file" | awk '{print $1}')

    # Compare checksums
    if [[ "$actual_checksum" != "$expected_checksum" ]]; then
        echo "ERROR: Downloaded MacOS SDK has an invalid checksum."
        exit 1
    fi

    echo "MacOS SDK downloaded successfully and has a valid checksum."
}

delete_artefacts() {
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

    echo "Deleting artefacts from ${WORKSPACE} ..."
    # delete possible artefacts from previous build(s)
    find ${WORKSPACE}/src \( -name "*.a" -o -name "*.la" -o -name "*.o" -o -name "*.lo" -o -name "*.Plo" -o -name "*.Po" -o -name "*.lai" -o -name "*.dirstamp" \) -delete
    find ${WORKSPACE}/src \( -name "*.a" -o -name "*.la" -o -name "*.o" -o -name "*.lo" -o -name "*.Plo" -o -name "*.Po" -o -name "*.lai" -o -name "*.dirstamp" \) -path "*/.*" -delete
    rm -f ${WORKSPACE}/src/qt/moc_*.cpp # delete meta object code files, otherwise we will have MacOS after Linux/Windows build error
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
        case $release_name in
        windows)
            bash -c "/usr/bin/x86_64-w64-mingw32-strip ${WORKSPACE}/${binary}${ext}" || false
            ;;
        macos)
            bash -c "${WORKSPACE}/depends/x86_64-apple-darwin/native/bin/x86_64-apple-darwin-strip ${WORKSPACE}/${binary}${ext}" || false
            ;;
        *)
            strip "${WORKSPACE}/${binary}${ext}" || false
            ;;
        esac
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
        windows)
            echo "Performing actions for Windows..."
            mv "${WORKSPACE}/releases/${release_name}/komodo-qt${ext}" "${WORKSPACE}/releases/${release_name}/komodo-qt-windows${ext}"
            ;;
        macos)
            echo "Performing actions for MacOS..."
            bash -c "make deploy" || false
            cp -f ${WORKSPACE}/*.dmg "${WORKSPACE}/releases/${release_name}/"
            mv "${WORKSPACE}/releases/${release_name}/komodo-qt${ext}" "${WORKSPACE}/releases/${release_name}/komodo-qt-mac${ext}"
            ;;
        *)
            echo "Unknown release name: $release_name"
            ;;
    esac
}

emulate_build() {
    for folder in macos windows focal; do
        mkdir -p ${WORKSPACE}/releases/${folder}
        for file in komodo-qt komodo-cli komodo-tx wallet-utility komodod; do
            extension=""
            case ${folder} in
                focal)
                    [[ "$file" == "komodo-qt" ]] && file=${file}-linux
                ;;
                macos)
                    [[ "$file" == "komodo-qt" ]] && file=${file}-mac
                    ;;
                windows)
                    extension=".exe"
                    [[ "$file" == "komodo-qt" ]] && file=${file}-windows
                    ;;
            esac
            echo test > ${WORKSPACE}/releases/${folder}/${file}${extension}
        done
    done
    echo test > ${WORKSPACE}/releases/macos/KomodoOcean-0.8.1-beta1.dmg
}

if true; then
    # Check if awk command exists
    command -v awk >/dev/null 2>&1 || { echo >&2 "ERROR: awk command not found."; exit 1; }
    # Check if sha256sum command exists
    command -v sha256sum >/dev/null 2>&1 || { echo >&2 "ERROR: sha256sum command not found."; exit 1; }

    ### focal
    if [[ "${build_focal}" = "true" ]]; then

        # delete old depends binaries (from previous linux version, bcz it's x86_64-unknown-linux-gnu also)
        if [[ "${delete_linux_depends}" = true ]]; then
            rm -rf ${WORKSPACE}/depends/built/x86_64-unknown-linux-gnu
            rm -rf ${WORKSPACE}/depends/x86_64-unknown-linux-gnu
            rm -rf ${WORKSPACE}/depends/built/x86_64-pc-linux-gnu
            rm -rf ${WORKSPACE}/depends/x86_64-pc-linux-gnu
        fi
        # delete possible artefacts from previous build(s)
        delete_artefacts focal
        bash -c 'zcutil/build.sh -j'$(expr $(nproc) - 1)
        copy_release focal
    fi

    ### windows
    if [[ "${build_windows}" = "true" ]]; then
        delete_artefacts windows
        bash -c 'zcutil/build-win.sh -j'$(expr $(nproc) - 1)
        copy_release windows
    fi

    ### macos
    if [[ "${build_macos}" = "true" ]]; then
        download_and_check_macos_sdk
        delete_artefacts macos
        bash -c 'zcutil/build-mac-cross.sh -j'$(expr $(nproc) - 1)
        copy_release macos
    fi
else
    emulate_build
    # all environment variables of docker container are accessible here,
    # you can use BUILDER_NAME or GITHUB_SHA, or GITHUB_ACTOR, etc.
fi
EOF


