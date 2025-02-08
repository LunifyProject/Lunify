# Multistage docker build, requires docker 17.05

# builder stage
FROM ubuntu:20.04 as builder

RUN set -ex && \
    apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get --no-install-recommends --yes install \
        automake \
        autotools-dev \
        bsdmainutils \
        build-essential \
        ca-certificates \
        ccache \
        cmake \
        curl \
        git \
        libtool \
        pkg-config \
        gperf

WORKDIR /src
COPY . .

ARG NPROC
RUN set -ex && \
    git submodule init && git submodule update && \
    rm -rf build && \
    if [ -z "$NPROC" ] ; \
    then make -j$(nproc) depends target=x86_64-linux-gnu ; \
    else make -j$NPROC depends target=x86_64-linux-gnu ; \
    fi

# runtime stage
FROM ubuntu:20.04

RUN set -ex && \
    apt-get update && \
    apt-get --no-install-recommends --yes install ca-certificates && \
    apt-get clean && \
    rm -rf /var/lib/apt
COPY --from=builder /src/build/x86_64-linux-gnu/release/bin /usr/local/bin/

# Create lunify user
RUN adduser --system --group --disabled-password lunify && \
	mkdir -p /wallet /home/lunify/.Lunify && \
	chown -R lunify:lunify /home/lunify/.Lunify && \
	chown -R lunify:lunify /wallet

# Contains the blockchain
VOLUME /home/lunify/.Lunify

# Generate your wallet via accessing the container and run:
# cd /wallet
# lunify-wallet-cli
VOLUME /wallet

EXPOSE 25251
EXPOSE 25252

# switch to user lunify
USER lunify

ENTRYPOINT ["lunifyd"]
CMD ["--p2p-bind-ip=0.0.0.0", "--p2p-bind-port=25251", "--rpc-bind-ip=0.0.0.0", "--rpc-bind-port=25252", "--non-interactive", "--confirm-external-bind"]

