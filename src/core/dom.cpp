#include <uniapi/dom.hpp>

#include <cstring>

namespace uniapi {

bool DomCommandWriter::Begin(Op op, size_t fieldBytes) {
    if (overflowed_ || size_ + 1 + fieldBytes > capacity_) {
        overflowed_ = true;
        return false;
    }
    PutU8(static_cast<uint8_t>(op));
    ++commands_;
    return true;
}

void DomCommandWriter::PutU8(uint8_t value) {
    data_[size_++] = value;
}

void DomCommandWriter::PutU32(uint32_t value) {
    for (int i = 0; i < 4; ++i) data_[size_++] = static_cast<uint8_t>(value >> (8 * i));
}

void DomCommandWriter::PutString(std::string_view text) {
    PutU32(static_cast<uint32_t>(text.size()));
    if (!text.empty()) std::memcpy(data_ + size_, text.data(), text.size());  // data() may be null
    size_ += text.size();
}

void DomCommandWriter::Bind(uint32_t id, std::string_view selector) {
    if (!Begin(Op::Bind, 4 + StringBytes(selector))) return;
    PutU32(id);
    PutString(selector);
}

void DomCommandWriter::Create(uint32_t id, std::string_view tag) {
    if (!Begin(Op::Create, 4 + StringBytes(tag))) return;
    PutU32(id);
    PutString(tag);
}

void DomCommandWriter::Append(uint32_t parent, uint32_t child) {
    if (!Begin(Op::Append, 8)) return;
    PutU32(parent);
    PutU32(child);
}

void DomCommandWriter::Remove(uint32_t id) {
    if (!Begin(Op::Remove, 4)) return;
    PutU32(id);
}

void DomCommandWriter::Clear(uint32_t id) {
    if (!Begin(Op::Clear, 4)) return;
    PutU32(id);
}

void DomCommandWriter::SetText(uint32_t id, std::string_view text) {
    if (!Begin(Op::SetText, 4 + StringBytes(text))) return;
    PutU32(id);
    PutString(text);
}

void DomCommandWriter::SetAttribute(uint32_t id, std::string_view name, std::string_view value) {
    if (!Begin(Op::SetAttribute, 4 + StringBytes(name) + StringBytes(value))) return;
    PutU32(id);
    PutString(name);
    PutString(value);
}

void DomCommandWriter::SetStyle(uint32_t id, std::string_view property, std::string_view value) {
    if (!Begin(Op::SetStyle, 4 + StringBytes(property) + StringBytes(value))) return;
    PutU32(id);
    PutString(property);
    PutString(value);
}

void DomCommandWriter::ToggleClass(uint32_t id, std::string_view name, bool on) {
    if (!Begin(Op::ToggleClass, 4 + StringBytes(name) + 1)) return;
    PutU32(id);
    PutString(name);
    PutU8(on ? 1 : 0);
}

}  // namespace uniapi
