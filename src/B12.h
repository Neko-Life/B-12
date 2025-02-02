#ifndef B12_B12_H_
#define B12_B12_H_

#define NOMINMAX

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <typeinfo>
#include <chrono>

#include <fmt/format.h>
#include <fmt/std.h>

#include <shion/types.h>
#include <shion/containers/registry.h>
#include <shion/io/logger.h>
#include <shion/traits/type_list.h>
#include <shion/traits/value_list.h>
#include <shion/utils/observer_ptr.h>
#include <shion/utils/string_literal.h>

#include <boost/pfr.hpp>

#include <magic_enum.hpp>

#include <dpp/dpp.h>

#include <source_location>

#ifdef ERROR
#  undef ERROR
#endif

using shion::string_literal;
using namespace shion::types;
using namespace std::string_view_literals;
using namespace std::chrono_literals;

namespace B12
{
	using shion::io::LogLevel;
	using nlohmann::json;

	using app_time = std::chrono::steady_clock::time_point;
	using file_time = std::chrono::file_clock::time_point;
	//	using utc_time = std::chrono::utc_clock::time_point;

	struct empty_t
	{};

	constexpr inline auto empty = empty_t{};

	struct value_init_t
	{
	};

	template <typename... Ts>
	struct invalid_t
	{
		consteval invalid_t()
		{
			static_assert(!(std::same_as<Ts, Ts> && ...));
		}
	};

	template <bool condition, auto lhs, auto rhs>
	constexpr inline auto conditional_v = invalid_t<decltype(lhs), decltype(rhs)>{};

	template <auto lhs, auto rhs>
	constexpr inline auto conditional_v<true, lhs, rhs> = lhs;

	template <auto lhs, auto rhs>
	constexpr inline auto conditional_v<false, lhs, rhs> = rhs;

	template <bool condition, typename Lhs, typename Rhs>
	constexpr auto conditional_fwd(Lhs&& lhs, Rhs&& rhs) -> decltype(auto)
	{
		if constexpr (condition)
			return (static_cast<Lhs&&>(lhs));
		else
			return (static_cast<Rhs&&>(rhs));
	}

	constexpr inline value_init_t value_init{};

	class Guild;
	struct CommandResponse;

	template <typename Lhs, typename Rhs>
	concept less_comparable_with = requires (Lhs lhs, Rhs rhs){{lhs < rhs} -> std::convertible_to<bool>;};

	template <typename InteractionType>
	static inline constexpr bool is_interaction_event = std::is_base_of_v<
		dpp::interaction_create_t, InteractionType>;

	inline constexpr auto to_upper = [](char &c)
	{
		if (c > 'a' && c < 'z')
			c += 'A' - 'a';
	};

	void log(LogLevel level, std::string_view str);

	bool isLogEnabled(LogLevel level);

	template <typename... Args>
		requires(sizeof...(Args) > 0)
	void log(LogLevel level, fmt::format_string<Args...> fmt, Args&&... args)
	{
		if (isLogEnabled(level))
			log(level, fmt::format(fmt, std::forward<Args>(args)...));
	}

	shion::utils::observer_ptr<Guild> fetchGuild(dpp::snowflake id);

	inline constexpr auto DISCORD_EPOCH = std::chrono::time_point<std::chrono::local_t, milliseconds>{
		1420070400000ms
	};

	using button_callback = CommandResponse(
		const dpp::button_click_t& event
	);

	constexpr inline auto is_whitespace = [](char c) constexpr {
		return (c == ' ' || c == '\t');
	};

	// Class to streamline the execution of D++'s async request API
	// TODO doc on how to use, in the meantime one can look at the Study command
	template <typename Arg, typename RestResult = dpp::confirmation_callback_t>
	class AsyncExecutor
	{
	public:
		using success_callback = void(const Arg&);
		using error_callback = void(const dpp::error_info& error);
		using rest_callback = void(const RestResult&);

	private:
		std::mutex              _mutex;
		std::condition_variable _cv;
		bool                    _complete{true};

		std::function<success_callback> _on_success = shion::noop;
		std::function<error_callback>   _on_error   = [](const dpp::error_info& error)
		{
			B12::log(B12::LogLevel::ERROR, "error while trying to execute async task: {}", error.message);
		};
		std::function<rest_callback> _rest_callback = [this](const RestResult& result)
		{
			std::unique_lock lock{_mutex};
			if constexpr (std::is_base_of_v<dpp::confirmation_callback_t, RestResult>)
			{
				if (result.is_error())
					_on_error(result.get_error());
				else
					_on_success(std::get<Arg>(result.value));
			}
			else if constexpr (std::is_base_of_v<dpp::http_request_completion_t, RestResult>)
			{
				const dpp::http_request_completion_t& res = result;

				if (res.error != dpp::h_success)
					_on_error(dpp::error_info{static_cast<uint32_t>(res.error), fmt::format("error code {}", static_cast<std::underlying_type_t<dpp::http_error>>(res.error)), {}});
				else
					_on_success(res);
			}
			_complete = true;
			_cv.notify_all();
		};

	public:
		using arg_type = Arg;

		AsyncExecutor()                = default;
		AsyncExecutor(AsyncExecutor&&) = default;

		explicit AsyncExecutor(std::invocable<const Arg&> auto&& success_callback) :
			_on_success(success_callback) {}

		AsyncExecutor(
			std::invocable<const Arg&> auto&&             success_callback,
			std::invocable<const dpp::error_info&> auto&& error_callback
		) :
			_on_success(success_callback),
			_on_error(error_callback) { }

		template <typename T, typename... Args>
			requires (std::invocable<T, Args..., decltype(_rest_callback)>)
		AsyncExecutor &operator()(T&& routine, Args&&... args)
		{
			std::scoped_lock lock(_mutex);
			assert(_complete);

			_complete = false;
			std::invoke(routine, std::forward<Args>(args)..., _rest_callback);
			return (*this);
		}

		~AsyncExecutor()
		{
			wait();
		}

		void wait()
		{
			std::unique_lock lock{_mutex};
			auto             wait = [this]()
			{
				return (_complete);
			};

			_cv.wait(lock, wait);
		}
	};

	inline std::string format_command_error(dpp::interaction_create_t const &event, std::string_view error, std::source_location loc = std::source_location::current()) {
		return (fmt::format(
			"error in {}: \n\t{}\n\tuser: {: >16}\tguild: {: >16}\tchannel: {: >16}",
			loc.function_name(), error, event.command.usr.id, event.command.guild_id, event.command.channel_id
		));
	}
} // namespace B12

#endif
