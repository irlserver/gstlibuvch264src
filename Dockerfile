# Use a ARM64 base image
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
	ninja-build \
	patch \
	pkg-config \
	libgstreamer1.0-dev \
	libgstreamer-plugins-base1.0-dev \
	libjpeg-dev \
	libusb-1.0-0 \
	libusb-1.0-0-dev

# Copy plugin sources and patches into the image
COPY . /app

# Build and install libuvc from upstream v0.0.7 with the H265 and UVC 1.5
# patches applied. Stock libuvc lacks UVC_FRAME_FORMAT_H265 (required by the
# plugin's H265 codepaths) and predates UVC 1.5 header parsing and the
# libusb auto-detach call needed to claim devices bound to the uvcvideo
# kernel driver.
WORKDIR /app/libuvc-build
RUN git clone --depth 1 --branch v0.0.7 https://github.com/libuvc/libuvc.git . && \
	patch -p1 < /app/patches/uvc15-support.patch && \
	patch -p1 < /app/patches/libuvc-h265-support.patch && \
	cmake . \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=ON \
		-DBUILD_EXAMPLE=OFF \
		-DBUILD_TEST=OFF && \
	make -j"$(nproc)" && \
	make install && \
	ldconfig

# Build and install libuvch264src
WORKDIR /app
RUN meson setup build ./libuvch264src/
WORKDIR /app/build
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
