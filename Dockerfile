# Multi-architecture support
ARG TARGETARCH

FROM ubuntu:latest AS build

# Set the working directory inside the container
WORKDIR /app

# Install essential build tools, GStreamer dependencies, and libusb
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
	build-essential \
	cmake \
	git \
	meson \
	pkg-config \
	libgstreamer1.0-dev \
	libgstreamer-plugins-base1.0-dev \
	libusb-1.0-0 \
	libusb-1.0-0-dev

# Clone the necessary repositories
RUN git clone https://github.com/irlserver/gstlibuvch264src.git .

# Build and install libuvc
WORKDIR /app/libuvc
RUN cmake .
RUN make -j$(nproc)
RUN make install

# Build and install libuvch264src
WORKDIR /app
RUN mkdir build
RUN meson setup build ./libuvch264src/
WORKDIR /app/build
RUN meson compile
RUN meson install --no-rebuild

# --- Second Stage: Create a smaller image for just the plugin ---
FROM ubuntu:latest AS runtime

# Multi-architecture support
ARG TARGETARCH

# Install runtime dependencies (GStreamer)
RUN apt-get update && apt-get install -y \
	libgstreamer1.0-0 \
	libgstreamer-plugins-base1.0-0 \
	libusb-1.0-0 \
	&& rm -rf /var/lib/apt/lists/*

# Set architecture-specific library path
# TARGETARCH is "arm64" or "amd64", map to GNU triplet
RUN GNUARCH=$(case "${TARGETARCH}" in \
	"arm64") echo "aarch64-linux-gnu" ;; \
	"amd64") echo "x86_64-linux-gnu" ;; \
	*) echo "unknown-linux-gnu" ;; \
	esac) && \
	mkdir -p /usr/local/lib/${GNUARCH}/gstreamer-1.0 && \
	mkdir -p /usr/lib/${GNUARCH} && \
	echo "${GNUARCH}" > /tmp/gnuarch

# Copy the built GStreamer plugin and libuvc from the build stage
RUN GNUARCH=$(cat /tmp/gnuarch) && \
	echo "Copying libraries for architecture: ${GNUARCH}"
COPY --from=build /usr/local/lib/*/gstreamer-1.0/libgstlibuvch264src.so /tmp/plugin.so
COPY --from=build /usr/local/lib/libuvc.* /tmp/
RUN GNUARCH=$(cat /tmp/gnuarch) && \
	mv /tmp/plugin.so /usr/local/lib/${GNUARCH}/gstreamer-1.0/ && \
	mv /tmp/libuvc.* /usr/lib/${GNUARCH}/
