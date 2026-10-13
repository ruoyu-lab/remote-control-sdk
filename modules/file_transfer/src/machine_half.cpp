#include "machine_half.hpp"

namespace rcsdk::file_transfer {

Machine::Machine(rcs_module module)
    : module_(module),
      worker_(
          [module](Reply reply) {
              Bytes bytes = encode(reply);
              rcs_result sent = rcs_module_send_machine(module, bytes.data(), uint32_t(bytes.size()));
              // A listing or a preparation too large for one message: the session's half hears why instead.
              const std::string why = "The answer is larger than one message of the module";
              if (const auto* listed = std::get_if<Listed>(&reply); listed && sent == RCS_ERROR_INVALID_ARGUMENT) {
                  bytes = encode(Reply{Listed{listed->job, RCS_ERROR_INVALID_ARGUMENT, why, {}, {}, {}}});
                  sent = rcs_module_send_machine(module, bytes.data(), uint32_t(bytes.size()));
              } else if (const auto* prepared = std::get_if<Prepared>(&reply);
                         prepared && sent == RCS_ERROR_INVALID_ARGUMENT) {
                  bytes = encode(Reply{Prepared{prepared->transfer, RCS_ERROR_INVALID_ARGUMENT, why, {}}});
                  sent = rcs_module_send_machine(module, bytes.data(), uint32_t(bytes.size()));
              }
              if (sent != RCS_OK) {
                  rcs_module_log(module, RCS_LOG_WARNING,
                                 ("An answer to the session's half did not go: " +
                                  std::string(rcs_result_message(sent)))
                                     .c_str());
              }
          },
          [module](const std::string& why) { rcs_module_log(module, RCS_LOG_WARNING, why.c_str()); }) {}

void Machine::fromSession(std::span<const uint8_t> data) {
    try {
        worker_.post(decodeRequest(data));
    } catch (const Error& error) {
        rcs_module_log(module_, RCS_LOG_ERROR,
                       (std::string("The session's half sent what this end does not read: ") + error.what()).c_str());
    }
}

} // namespace rcsdk::file_transfer
