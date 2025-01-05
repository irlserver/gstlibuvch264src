# Use a base image with necessary build tools and GStreamer dependencies for ARM64
FROM --platform=linux/arm64 ubuntu:latest AS build

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

# Clone the necessary repositories (assuming libuvch264src depends on libuvc)
RUN git clone https://github.com/libuvc/libuvc.git
RUN git clone https://github.com/irlserver/gstlibuvch264src.git

# Build and install libuvc
WORKDIR /app/libuvc
RUN cmake .
RUN make -j$(nproc)
RUN make install

# Build and install libuvch264src
WORKDIR /app
RUN mkdir build
WORKDIR /app/build
RUN meson setup ../libuvch264src/
RUN meson compile
RUN meson install --no-rebuild

# --- Second Stage: Create a smaller image for just the plugin ---
FROM --platform=linux/arm64 ubuntu:latest AS runtime

# Install runtime dependencies (GStreamer)
RUN apt-get update && apt-get install -y \
	libgstreamer1.0-0 \
	libgstreamer-plugins-base1.0-0 \
	libusb-1.0-0 \
	&& rm -rf /var/lib/apt/lists/*

# Create necessary directories
RUN mkdir -p /usr/local/lib/aarch64-linux-gnu/gstreamer-1.0
RUN mkdir -p /usr/lib/aarch64-linux-gnu

# Copy the built GStreamer plugin and libuvc from the build stage
COPY --from=build /usr/local/lib/aarch64-linux-gnu/gstreamer-1.0/libgstlibuvch264src.so /usr/local/lib/aarch64-linux-gnu/gstreamer-1.0/
COPY --from=build /usr/local/lib/libuvc.* /usr/lib/aarch64-linux-gnu/
