# The forwarder in a container, built from this repository (Ubuntu's
# compiler and CMake are all it installs; docs/guide/24-forwarder.md):
#   docker build -f server/forwarder/deploy/Dockerfile -t rcs-forwarder .
#   docker run -d --name rcs-forwarder --restart unless-stopped \
#       -p 47995:47995/udp -p 47995:47995/tcp -p 127.0.0.1:47996:47996/tcp \
#       -v /etc/rcs-forwarder:/etc/rcs-forwarder:ro rcs-forwarder
# Inside, the configuration listens on 0.0.0.0 (the WebSocket entry's port is
# published on the host's loopback, for the TLS proxy there), and its files
# are readable by the account 990:990 (chown root:990, chmod 0640; the
# directory 0750). The event lines go to standard output: docker logs.
# `docker stop` sends SIGTERM: the ends hear the forwarder goes.

FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends cmake ninja-build g++ \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json LICENSE THIRD_PARTY_NOTICES.md ./
COPY cmake cmake
COPY include include
COPY src src
COPY third_party third_party
COPY modules modules
COPY components components
COPY server server
RUN cmake --preset linux-forwarder -DBUILD_TESTING=OFF \
 && cmake --build build/linux-forwarder --target rcs_forwarder \
 && cmake --install build/linux-forwarder --component forwarder --prefix /opt/rcs-forwarder

FROM ubuntu:24.04
RUN groupadd --system --gid 990 rcs-forwarder \
 && useradd --system --uid 990 --gid 990 --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin \
            rcs-forwarder
COPY --from=build /opt/rcs-forwarder/bin/rcs_forwarder /usr/local/bin/
COPY --from=build /opt/rcs-forwarder/lib/librcsfwd.so* /usr/local/lib/
COPY --from=build /opt/rcs-forwarder/share/rcs-forwarder/ /usr/local/share/rcs-forwarder/
USER rcs-forwarder
EXPOSE 47995/udp 47995/tcp 47996/tcp
ENTRYPOINT ["/usr/local/bin/rcs_forwarder"]
CMD ["--config", "/etc/rcs-forwarder/forwarder.conf"]
