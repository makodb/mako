#pragma once

// Included in the exported module epilogue after the canonical declarations.
// The generic bridges and STL trait adapters provide C++ call dispatch only.
// Canonical Rust functions own byte handling and collection traversal.
namespace srpc {

namespace Serialize_ {
namespace adl_detail_ { void serialize(); }
template<class T>
void adl_serialize_bridge(const T& value, BinaryWriteArchive& archive) {
    rusty::srpc_adl_serialize(value, archive);
}
}
namespace Deserialize_ {
namespace adl_detail_ { void deserialize(); }
template<class T>
void adl_deserialize_bridge(T& value, BinaryReadArchive& archive) {
    rusty::srpc_adl_deserialize(value, archive);
}
}

// Match the generated adapters' recursive trait bounds. Container shape alone
// does not make an unsupported element serializable.
template<class T> inline constexpr bool stl_serialize_adapter = [] {
    if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>) {
        return true;
    } else if constexpr (rusty::srpc_stl_pair<T>) {
        return (requires { sizeof(SerializeAdapter<typename T::first_type>); }
                || std::is_base_of_v<Serialize, typename T::first_type>)
            && (requires { sizeof(SerializeAdapter<typename T::second_type>); }
                || std::is_base_of_v<Serialize, typename T::second_type>);
    } else if constexpr (rusty::srpc_stl_map<T>) {
        return (requires { sizeof(SerializeAdapter<typename T::key_type>); }
                || std::is_base_of_v<Serialize, typename T::key_type>)
            && (requires { sizeof(SerializeAdapter<typename T::mapped_type>); }
                || std::is_base_of_v<Serialize, typename T::mapped_type>);
    } else if constexpr (rusty::srpc_stl_sequence<T>) {
        return requires { sizeof(SerializeAdapter<typename T::value_type>); }
            || std::is_base_of_v<Serialize, typename T::value_type>;
    } else return false;
}();

template<class T> inline constexpr bool stl_deserialize_key = [] {
    if constexpr (requires { typename T::key_compare; }) {
        return std::totally_ordered<typename T::key_type>;
    } else if constexpr (requires { typename T::hasher; }) {
        return std::equality_comparable<typename T::key_type>
            && requires(const typename T::key_type& key) { std::hash<typename T::key_type>{}(key); };
    } else return true;
}();

template<class T> inline constexpr bool stl_deserialize_adapter = [] {
    if constexpr (std::is_same_v<T, std::string>) {
        return true;
    } else if constexpr (rusty::srpc_stl_pair<T>) {
        return (requires { sizeof(DeserializeAdapter<typename T::first_type>); }
                || std::is_base_of_v<Deserialize, typename T::first_type>)
            && (requires { sizeof(DeserializeAdapter<typename T::second_type>); }
                || std::is_base_of_v<Deserialize, typename T::second_type>);
    } else if constexpr (rusty::srpc_stl_map<T>) {
        return (requires { sizeof(DeserializeAdapter<typename T::key_type>); }
                || std::is_base_of_v<Deserialize, typename T::key_type>)
            && (requires { sizeof(DeserializeAdapter<typename T::mapped_type>); }
                || std::is_base_of_v<Deserialize, typename T::mapped_type>)
            && std::default_initializable<typename T::key_type>
            && std::default_initializable<typename T::mapped_type>
            && stl_deserialize_key<T>;
    } else if constexpr (rusty::srpc_stl_sequence<T>) {
        return (requires { sizeof(DeserializeAdapter<typename T::value_type>); }
                || std::is_base_of_v<Deserialize, typename T::value_type>)
            && std::default_initializable<typename T::value_type>
            && stl_deserialize_key<T>;
    } else return false;
}();

template<class T> requires stl_serialize_adapter<T>
class SerializeAdapter<T> final : public Serialize {
    T value_;
public:
    SerializeAdapter(T value) : value_(std::move(value)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& archive) const override {
        rusty::srpc_adl_serialize(value_, archive);
    }
};

template<class T> requires stl_serialize_adapter<T>
class SerializeAdapterRef<T> final : public Serialize {
    const T& value_;
public:
    explicit SerializeAdapterRef(const T& value) : value_(value) {}
    void serialize(BinaryWriteArchive& archive) const override {
        rusty::srpc_adl_serialize(value_, archive);
    }
};

template<class T> requires stl_serialize_adapter<T>
class SerializeAdapterRefMut<T> final : public Serialize {
    T& value_;
public:
    explicit SerializeAdapterRefMut(T& value) : value_(value) {}
    void serialize(BinaryWriteArchive& archive) const override {
        rusty::srpc_adl_serialize(value_, archive);
    }
};

template<class T> requires stl_deserialize_adapter<T>
class DeserializeAdapter<T> final : public Deserialize {
    T value_;
public:
    DeserializeAdapter(T value) : value_(std::move(value)) {}
    DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
    void deserialize(BinaryReadArchive& archive) override {
        rusty::srpc_adl_deserialize(value_, archive);
    }
};

template<class T> requires stl_deserialize_adapter<T>
class DeserializeAdapterRef<T> final : public Deserialize {
    const T& value_;
public:
    explicit DeserializeAdapterRef(const T& value) : value_(value) {}
    void deserialize(BinaryReadArchive&) override {
        std::abort();  // A const reference cannot expose mutable trait dispatch.
    }
};

template<class T> requires stl_deserialize_adapter<T>
class DeserializeAdapterRefMut<T> final : public Deserialize {
    T& value_;
public:
    explicit DeserializeAdapterRefMut(T& value) : value_(value) {}
    void deserialize(BinaryReadArchive& archive) override {
        rusty::srpc_adl_deserialize(value_, archive);
    }
};

// Preserve the established non-template entry points and concrete adapter ABI.
// Each body forwards to the same canonical helpers used by the STL templates.
namespace Serialize_ {
void serialize(const std::string& value, BinaryWriteArchive& archive) {
    ::srpc::serialize<BinaryWriteArchive>(value, archive);
}
void serialize(const std::string_view& value, BinaryWriteArchive& archive) {
    ::srpc::serialize<BinaryWriteArchive>(value, archive);
}
}
namespace Deserialize_ {
void deserialize(std::string& value, BinaryReadArchive& archive) {
    ::srpc::deserialize<BinaryReadArchive>(value, archive);
}
}
namespace rusty_ext {
void serialize(const std::string& value, BinaryWriteArchive& archive) {
    Serialize_::serialize(value, archive);
}
void serialize(const std::string_view& value, BinaryWriteArchive& archive) {
    Serialize_::serialize(value, archive);
}
void deserialize(std::string& value, BinaryReadArchive& archive) {
    Deserialize_::deserialize(value, archive);
}
}

template<>
class SerializeAdapter<std::string> final : public Serialize {
    std::string value_;
public:
    SerializeAdapter(std::string value);
    SerializeAdapter(SerializeAdapter&& other);
    void serialize(BinaryWriteArchive& archive) const override;
};
SerializeAdapter<std::string>::SerializeAdapter(std::string value) : value_(std::move(value)) {}
SerializeAdapter<std::string>::SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
void SerializeAdapter<std::string>::serialize(BinaryWriteArchive& archive) const { Serialize_::serialize(value_, archive); }

template<>
class SerializeAdapterRef<std::string> final : public Serialize {
    const std::string& value_;
public:
    explicit SerializeAdapterRef(const std::string& value);
    void serialize(BinaryWriteArchive& archive) const override;
};
SerializeAdapterRef<std::string>::SerializeAdapterRef(const std::string& value) : value_(value) {}
void SerializeAdapterRef<std::string>::serialize(BinaryWriteArchive& archive) const { Serialize_::serialize(value_, archive); }

template<>
class SerializeAdapterRefMut<std::string> final : public Serialize {
    std::string& value_;
public:
    explicit SerializeAdapterRefMut(std::string& value);
    void serialize(BinaryWriteArchive& archive) const override;
};
SerializeAdapterRefMut<std::string>::SerializeAdapterRefMut(std::string& value) : value_(value) {}
void SerializeAdapterRefMut<std::string>::serialize(BinaryWriteArchive& archive) const { Serialize_::serialize(value_, archive); }

template<>
class SerializeAdapter<std::string_view> final : public Serialize {
    std::string_view value_;
public:
    SerializeAdapter(std::string_view value);
    SerializeAdapter(SerializeAdapter&& other);
    void serialize(BinaryWriteArchive& archive) const override;
};
SerializeAdapter<std::string_view>::SerializeAdapter(std::string_view value) : value_(std::move(value)) {}
SerializeAdapter<std::string_view>::SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
void SerializeAdapter<std::string_view>::serialize(BinaryWriteArchive& archive) const { Serialize_::serialize(value_, archive); }

template<>
class SerializeAdapterRef<std::string_view> final : public Serialize {
    const std::string_view& value_;
public:
    explicit SerializeAdapterRef(const std::string_view& value);
    void serialize(BinaryWriteArchive& archive) const override;
};
SerializeAdapterRef<std::string_view>::SerializeAdapterRef(const std::string_view& value) : value_(value) {}
void SerializeAdapterRef<std::string_view>::serialize(BinaryWriteArchive& archive) const { Serialize_::serialize(value_, archive); }

template<>
class SerializeAdapterRefMut<std::string_view> final : public Serialize {
    std::string_view& value_;
public:
    explicit SerializeAdapterRefMut(std::string_view& value);
    void serialize(BinaryWriteArchive& archive) const override;
};
SerializeAdapterRefMut<std::string_view>::SerializeAdapterRefMut(std::string_view& value) : value_(value) {}
void SerializeAdapterRefMut<std::string_view>::serialize(BinaryWriteArchive& archive) const { Serialize_::serialize(value_, archive); }

template<>
class DeserializeAdapter<std::string> final : public Deserialize {
    std::string value_;
public:
    DeserializeAdapter(std::string value);
    DeserializeAdapter(DeserializeAdapter&& other);
    void deserialize(BinaryReadArchive& archive) override;
};
DeserializeAdapter<std::string>::DeserializeAdapter(std::string value) : value_(std::move(value)) {}
DeserializeAdapter<std::string>::DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
void DeserializeAdapter<std::string>::deserialize(BinaryReadArchive& archive) { Deserialize_::deserialize(value_, archive); }

template<>
class DeserializeAdapterRef<std::string> final : public Deserialize {
    const std::string& value_;
public:
    explicit DeserializeAdapterRef(const std::string& value);
    void deserialize(BinaryReadArchive& archive) override;
};
DeserializeAdapterRef<std::string>::DeserializeAdapterRef(const std::string& value) : value_(value) {}
void DeserializeAdapterRef<std::string>::deserialize(BinaryReadArchive&) { std::abort(); }

template<>
class DeserializeAdapterRefMut<std::string> final : public Deserialize {
    std::string& value_;
public:
    explicit DeserializeAdapterRefMut(std::string& value);
    void deserialize(BinaryReadArchive& archive) override;
};
DeserializeAdapterRefMut<std::string>::DeserializeAdapterRefMut(std::string& value) : value_(value) {}
void DeserializeAdapterRefMut<std::string>::deserialize(BinaryReadArchive& archive) { Deserialize_::deserialize(value_, archive); }

}  // namespace srpc
