#pragma once

#include <functional>
#include <memory>
#include <string_view>

namespace wf
{
namespace signal
{
class provider_t;

/**
 * A connection to a signal on an object.
 * Uses RAII to automatically disconnect the signal when it goes out of scope.
 */
template<class SignalType>
class connection_t final
{
  public:
    using callback = std::function<void (SignalType*)>;
    /** Initialize an empty signal connection */
    connection_t();
    /** Automatically disconnects from all providers */
    ~connection_t();

    template<class CallbackType> using convertible_to_callback_t =
        std::enable_if_t<std::is_constructible_v<callback, CallbackType>, void>;

    /** Initialize a signal connection with the given callback */
    template<class T, class U = convertible_to_callback_t<T>>
    connection_t(T callback) : connection_t()
    {
        set_callback(callback);
    }

    /** Set the signal callback or override the existing signal callback. */
    void set_callback(callback cb);

    /** Call the stored callback with the given data. */
    void emit(SignalType& data);

    /** Disconnect from all connected signal providers */
    void disconnect();

    class impl;
    std::unique_ptr<impl> priv;

  private:
    // Non-copyable and non-movable, as that would require updating/duplicating
    // the signal handler. But this is usually not what users of this API want.
    // Also provider_t holds pointers to this object.
    connection_t(const connection_t&) = delete;
    connection_t(connection_t&&) = delete;
    connection_t& operator =(const connection_t&) = delete;
    connection_t& operator =(connection_t&&) = delete;
};

template<class SignalType>
struct determine_signal_name
{};

class provider_t
{
  public:
    /**
     * Signals are designed to be useful for C++ plugins, however, they are
     * generally quite difficult to
     *  using c_api_callback = std::function<void(void*, void*, const char*)>;
     *
     *
     *  /** Register a connection to be called when the given signal is emitted. */
    template<class SignalType>
    void connect_signal(connection_t<SignalType> *callback);
    /** Unregister a connection. */
    template<class SignalType>
    void disconnect_signal(connection_t<SignalType> *callback);

    /** Emit the given signal. */
    template<class SignalType>
    void emit_signal(SignalType *data, std::string_view name = SignalType::name)
    {}

    provider_t();
    virtual ~provider_t();

    // Non-movable, non-copyable: connection_t keeps reference to this object.
    // Unclear what happens if this object is duplicated, and plugins usually
    // don't want this either.
    provider_t(const provider_t& other) = delete;
    provider_t& operator =(const provider_t& other) = delete;
    provider_t(provider_t&& other) = delete;
    provider_t& operator =(provider_t&& other) = delete;

  private:
    class sprovider_impl;
    std::unique_ptr<sprovider_impl> sprovider_priv;
};
}
}
