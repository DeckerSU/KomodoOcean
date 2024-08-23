# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Build:
# docker build -f Dockerfile -t komodoocean .

# Prepare data directory and config file:
# mkdir -p ./komodo-data/.komodo && RANDPASS=$(tr -cd '[:alnum:]' < /dev/urandom | fold -w16 | head -n1) && echo -e "txindex=1\nrpcuser=komodo\nrpcpassword=${RANDPASS}\nrpcallowip=127.0.0.1\nrpcbind=127.0.0.1\nserver=1" > ./komodo-data/.komodo/komodo.conf
# docker run --rm -v "$(pwd)/komodo-data:/data" ubuntu:18.04 bash -c 'chown -R nobody:nogroup /data'

# Run:
# docker run -d --rm -p 7770:7770 -v "$(pwd)/komodo-data:/data" komodoocean -printtoconsole=1

# Run AC (for example VOTE2024):
# mkdir -p ./VOTE2024/.komodo
# docker run --rm -v "$(pwd)/VOTE2024:/data" ubuntu:18.04 bash -c 'chown -R nobody:nogroup /data'
# docker run -d --rm -v "$(pwd)/VOTE2024:/data" komodoocean -printtoconsole=1 -ac_name=VOTE2024 -ac_public=1 -ac_supply=149826699 -ac_staked=10 -addnode=65.21.52.18

# Access the container after it's already running:
# docker ps --filter ancestor=komodoocean # find the id / name of container
# docker exec -it container_id_or_name /bin/bash # run bash inside the container
# docker exec -it container_id_or_name /app/komodo-cli getinfo  # run RPC getinfo

# Run container with current user UID:GID:
# mkdir -p ./komodo-data/.komodo && RANDPASS=$(tr -cd '[:alnum:]' < /dev/urandom | fold -w16 | head -n1) && echo -e "txindex=1\nrpcuser=komodo\nrpcpassword=${RANDPASS}\nrpcallowip=127.0.0.1\nrpcbind=127.0.0.1\nserver=1" > ./komodo-data/.komodo/komodo.conf
# docker run --rm -d --user $(id -u):$(id -g) -e HOME=/data -v "$(pwd)/komodo-data:/data" komodoocean -no-ownership-check -printtoconsole

# Run AC with current user UID:GID:
# mkdir -p ./VOTE2024/.komodo # pay attention that existance of .komodo folder is important (!)
# docker run --rm -d --user $(id -u):$(id -g) -e HOME=/data -v "$(pwd)/VOTE2024:/data" komodoocean -no-ownership-check -printtoconsole -ac_name=VOTE2024 -ac_public=1 -ac_supply=149826699 -ac_staked=10 -addnode=65.21.52.182

## Build komodod
FROM ubuntu:20.04 as komodod-builder
LABEL maintainer="DeckerSU <deckersu@protonmail.com>"

SHELL ["/bin/bash", "-c"]

# Latest release komodo v0.9.0-beta1
ARG KOMODO_COMMITTISH=v0.9.0-beta1
ARG IS_RELEASE=false
# DeckerSU <deckersu@protonmail.com> https://keys.openpgp.org/vks/v1/by-fingerprint/FD9A772C7300F4C894D1A819FE50480862E6451C
ARG KOMODOD_MAINTAINER_KEYS="FD9A772C7300F4C894D1A819FE50480862E6451C"

RUN set -euxo pipefail \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get update \
    && apt-get -y --no-install-recommends install apt-utils \
    && apt-get -y --no-install-recommends dist-upgrade \
    && apt-get -y --no-install-recommends install autoconf automake \
      bsdmainutils build-essential ca-certificates cmake curl dirmngr fakeroot \
      git g++-multilib gnupg2 libc6-dev libgomp1 libtool m4 ncurses-dev \
      pkg-config python3 zlib1g-dev \
    && git clone https://github.com/DeckerSU/KomodoOcean.git \
    && cd /KomodoOcean && git checkout "${KOMODO_COMMITTISH}" \
    && if [ "$IS_RELEASE" = "true" ]; then \
      read -a keys <<< "$KOMODOD_MAINTAINER_KEYS" \
      && for key in "${keys[@]}"; do \
        gpg2 --batch --keyserver keyserver.ubuntu.com --keyserver-options timeout=15 --recv "$key" || \
        gpg2 --batch --keyserver hkps://keys.openpgp.org --keyserver-options timeout=15 --recv "$key" || \
        gpg2 --batch --keyserver hkp://p80.pool.sks-keyservers.net:80 --keyserver-options timeout=15 --recv-keys "$key" || \
        gpg2 --batch --keyserver hkp://ipv4.pool.sks-keyservers.net --keyserver-options timeout=15 --recv-keys "$key" || \
        gpg2 --batch --keyserver hkp://pgp.mit.edu:80 --keyserver-options timeout=15 --recv-keys "$key"; \
      done \
      && if git verify-tag -v "${KOMODO_COMMITTISH}"; then \
        echo "Valid signed tag"; \
      else \
        echo "Not a valid signed tag"; \
        exit 1; \
      fi \
      && ( gpgconf --kill dirmngr || true ) \
      && ( gpgconf --kill gpg-agent || true ); \
    fi \
    && export MAKEFLAGS="-j $(($(nproc)-1))" && ./zcutil/build-no-qt.sh $MAKEFLAGS

## Build Final Image
FROM ubuntu:20.04

LABEL maintainer="DeckerSU <deckersu@protonmail.com>"

SHELL ["/bin/bash", "-c"]

WORKDIR /app

# Copy komodod and fetch-params.sh
COPY --from=komodod-builder /KomodoOcean/src/komodod /KomodoOcean/src/komodo-cli /KomodoOcean/zcutil/fetch-params.sh /app/
# Copy entrypoint script
COPY entrypoint.sh /app

# Install runtime dependencies and set up home folder for nobody user.
# As it is best practice to not run as root even inside a container,
# we run as nobody and change the home folder to "/data".

RUN set -euxo pipefail \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get update \
    && apt-get -y --no-install-recommends install apt-utils \
    && apt-get -y --no-install-recommends dist-upgrade \
    && apt-get -y --no-install-recommends install ca-certificates curl libgomp1 \
    && apt-get -y clean && apt-get -y autoclean \
    && rm -rf /var/{lib/apt/lists/*,cache/apt/archives/*.deb,tmp/*,log/*} /tmp/* \
    && mkdir -p /data \
    && for path in /data /app; do chown -R nobody:nogroup $path && chmod 2755 $path; done \
    && for file in /app/{fetch-params.sh,komodod,komodo-cli}; do chmod 755 $file; done \
    && sed -i 's|nobody:/nonexistent|nobody:/data|' /etc/passwd

VOLUME ["/data"]

USER nobody

ENTRYPOINT ["/app/entrypoint.sh"]

