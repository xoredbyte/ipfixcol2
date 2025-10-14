# Use an official Ubuntu 20.04 as a parent image
FROM ubuntu:24.04

# Set environment variables
ENV BUILD_DIR="/build"
ENV INSTALL_DIR="/usr/local"
ENV CMAKE_BUILD_TYPE="Release"
ENV TZ="Asia/Kolkata"
ENV DEBIAN_FRONTEND="noninteractive"
# Install dependencies needed for ipfixcol2

RUN apt update && apt install -y curl wget gpg gnupg2 software-properties-common

RUN ln -fs /usr/share/zoneinfo/$TZ /etc/localtime && \
echo $TZ > /etc/timezone && \
    apt update && \
    apt-get install -y \
    iproute2 \
    tcpdump \
    htop \
    iotop \
    software-properties-common \
    cmake \
    curl \
    net-tools \
    make \
    build-essential \
    libpcap-dev \
    libssl-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libxml2-dev \
    liblz4-dev \
    libzstd-dev \
    git \
    pkg-config \
    python3-docutils \
    zlib1g-dev \
    librdkafka-dev \
    doxygen \
    pkg-config \
    libcurl4-openssl-dev \
    libboost-system-dev \
    libboost-filesystem-dev \
    libboost-program-options-dev \
    libmicrohttpd12 libmicrohttpd-dev \
    && rm -rf /var/lib/apt/lists/*

# Clone and build libfds (a required dependency)
RUN git clone https://github.com/CESNET/libfds.git /libfds \
    && cd /libfds \
    && mkdir -p build && cd build \
    && cmake .. -DCMAKE_INSTALL_PREFIX=/usr \
    && make \   
    && make install

# Copy local ipfixcol2 source code into the container
COPY ./ ./ipfixcol2/ 

# Build and install ipfixcol2
WORKDIR /ipfixcol2
RUN rm -rf build && mkdir build && cd build && cmake .. && make && make install
COPY ./startup.xml /usr/local/etc/ipfixcol2/startup.xml
COPY ./sample.ipfix ./ipfixcol2/ 
COPY ./ipfixsend.sh ./ipfixcol2/ 



# Expose necessary ports for ipfixcol2
EXPOSE 4739 9995
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# Set the entrypoint
ENTRYPOINT ["/entrypoint.sh"]

#Test Change