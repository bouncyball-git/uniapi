#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uniapi {

// Writes DOM commands as a compact binary list; uniapi.js applies a whole list in one
// go, so the browser restyles and repaints once per list. Use App::sendDom().
//
// Elements are addressed by numeric IDs chosen by the caller: Create or Bind assigns
// an ID, later commands refer to it. ID 0 is always document.body.
//
// Wire format, little-endian:
//   command = opcode:u8, then its fields
//   id      = u32
//   string  = length:u32 + UTF-8 bytes
//   bool    = u8 (0 or 1)
//
// Commands that do not fit in the remaining space are not written; overflowed()
// then returns true. Everything written before that is complete and valid.
class DomCommandWriter {
public:
    enum class Op : uint8_t {
        Bind = 1,          // id, selector   element matching a CSS selector
        Create = 2,        // id, tag        new detached element
        Append = 3,        // parent, child
        Remove = 4,        // id             detach and forget the ID (IDs inside it stay valid)
        Clear = 5,         // id             remove all children
        SetText = 6,       // id, text
        SetAttribute = 7,  // id, name, value
        SetStyle = 8,      // id, property, value   (CSS property name, e.g. "height")
        ToggleClass = 9,   // id, name, on
    };

    static constexpr uint32_t kBody = 0;

    DomCommandWriter(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity) {}

    void Bind(uint32_t id, std::string_view selector);
    void Create(uint32_t id, std::string_view tag);
    void Append(uint32_t parent, uint32_t child);
    void Remove(uint32_t id);
    void Clear(uint32_t id);
    void SetText(uint32_t id, std::string_view text);
    void SetAttribute(uint32_t id, std::string_view name, std::string_view value);
    void SetStyle(uint32_t id, std::string_view property, std::string_view value);
    void ToggleClass(uint32_t id, std::string_view name, bool on);

    size_t size() const { return size_; }
    uint32_t commands() const { return commands_; }
    bool overflowed() const { return overflowed_; }

private:
    bool Begin(Op op, size_t fieldBytes);  // reserves the whole command or marks overflow
    void PutU8(uint8_t value);
    void PutU32(uint32_t value);
    void PutString(std::string_view text);
    static size_t StringBytes(std::string_view text) { return 4 + text.size(); }

    uint8_t* data_;
    size_t capacity_;
    size_t size_ = 0;
    uint32_t commands_ = 0;
    bool overflowed_ = false;
};

}  // namespace uniapi
