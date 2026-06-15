# Multi-architecture support (driven by buildx --platform; CI builds amd64 + arm64)
ARG TARGETARCH

# Base pinned to a digest (not the mutable :latest / floating :24.04 tag) so the
# build is reproducible and verifiable offline. Refresh via:
#   docker buildx imagetools inspect ubuntu:24.04   (use the index Digest)
FROM ubuntu:24.04@sha256:786a8b558f7be160c6c8c4a54f9a57274f3b4fb1491cf65146521ae77ff1dc54 AS build

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
# Pinned to the exact commit the v0.0.7 tag pointed to (tags are mutable; a SHA
# is not). Shallow-fetch the single commit. Keep in sync with CMakeLists.txt.
RUN git init -q . && \
	git remote add origin https://github.com/libuvc/libuvc.git && \
	git fetch --depth 1 origin 68d07a00e11d1944e27b7295ee69673239c00b4b && \
	git checkout -q FETCH_HEAD && \
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
# Same pinned digest as the build stage (see note above).
FROM ubuntu:24.04@sha256:786a8b558f7be160c6c8c4a54f9a57274f3b4fb1491cf65146521ae77ff1dc54 AS runtime

# Multi-architecture support
ARG TARGETARCH

# Install runtime dependencies (GStreamer)
RUN apt-get update && apt-get install -y \
	libgstreamer1.0-0 \
	libgstreamer-plugins-base1.0-0 \
	libusb-1.0-0 \
	&& rm -rf /var/lib/apt/lists/*

# Map TARGETARCH ("arm64"/"amd64") to the GNU multiarch triplet and create the
# destination directory under /usr. Everything must live under /usr so the
# build-check workflow finds build/usr/lib/*/gstreamer-1.0/ and the release
# workflow packages build/usr/=/usr/.
RUN GNUARCH=$(case "${TARGETARCH}" in \
	"arm64") echo "aarch64-linux-gnu" ;; \
	"amd64") echo "x86_64-linux-gnu" ;; \
	*) echo "Unsupported architecture: '${TARGETARCH}' (expected arm64 or amd64)" >&2; exit 1 ;; \
	esac) && \
	mkdir -p /usr/lib/${GNUARCH}/gstreamer-1.0 && \
	echo "${GNUARCH}" > /tmp/gnuarch

# Copy the built GStreamer plugin and libuvc from the build stage. meson
# installs the plugin under /usr/local/lib/<triplet>/gstreamer-1.0/ and libuvc
# installs flat under /usr/local/lib/; relocate both under /usr/lib/<triplet>/
# while preserving the libgstlibuvch264src.so / libuvc.so* file names.
COPY --from=build /usr/local/lib/*/gstreamer-1.0/libgstlibuvch264src.so /tmp/stage/
COPY --from=build /usr/local/lib/libuvc.* /tmp/stage/
RUN GNUARCH=$(cat /tmp/gnuarch) && \
	mv /tmp/stage/libgstlibuvch264src.so /usr/lib/${GNUARCH}/gstreamer-1.0/ && \
	mv /tmp/stage/libuvc.* /usr/lib/${GNUARCH}/ && \
	rm -rf /tmp/stage /tmp/gnuarch
