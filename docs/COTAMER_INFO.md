2
For those of you hooking up your consensus to a real-world system, here are some updates to Cotamer I/O. You get these if you git pull handout main.

If you use Docker, the Dockerfile has been updated to install additional libraries required to compile Cotamer with HTTP, JSON, and libcurl support. cd docker; ./build-docker --no-cache

If you don’t use Docker, but you use Homebrew, brew install llhttp curl nlohmann-json wslay mbedtls

The return value from functions like cot::read and cot::write has changed. As I wrote more complex example programs, these functions’ exception behavior became really annoying. So they now return std::expected<size_t, std::error_code>. Writeup in the Cotamer I/O manual.https://read.seas.harvard.edu/cs2620/2026/cotamer-io/

I’ve written and documented some example code showing JSON-over-HTTP clients and servers. See here: https://github.com/readablesystems/cs2620-s26-psets/tree/main/examples

Programs are jsond (JSON server) and jsond-client (JSON client). You can run them by opening two terminals; cd cs2620-26-psets/examples; make, then in one terminal, run ./jsond, and in the other, run ./jsond-client.

The HTTP functions are written up here.

More to come!More updates: UDP support is now available, with examples in the I/O manual and checked in to examples/ in the psets handout repository.


https://read.seas.harvard.edu/cs2620/2026/cotamer-io/#udp