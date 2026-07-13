#ifndef CO_MIRA_GENERATOR_HPP
#define CO_MIRA_GENERATOR_HPP

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <ranges>
#include <type_traits>
#include <utility>
#include <variant>

namespace mira::co {

template <std::ranges::range Range> struct elements_of {
  Range range;
};

template <typename Range> elements_of(Range &&) -> elements_of<Range &&>;

template <typename Ref, typename Val = void> class generator;

template <typename Ref, typename Val>
using reference_t = std::conditional_t<std::is_void_v<Val>, Ref &&, Ref>;

template <typename Ref>
using yield_t = std::conditional_t<std::is_reference_v<Ref>, Ref, const Ref &>;

template <typename Ref, typename Val> using yield2_t = yield_t<reference_t<Ref, Val>>;

namespace core::gen {
template <typename Yielded> class promise_erased {
  static_assert(std::is_reference_v<Yielded>);

  using yielded_deref = std::remove_reference_t<Yielded>;
  using yielded_decvref = std::remove_cvref_t<Yielded>;
  using value_ptr = std::add_pointer_t<Yielded>;
  using co_handle = std::coroutine_handle<promise_erased>;

  struct subyield_state;
  struct copy_awaiter;
  struct final_awaiter;
  template <typename Gen> struct recursive_awatier;
  template <typename Ref, typename Val> friend class mira::co::generator;

public:
  static constexpr std::suspend_always initial_suspend() noexcept { return {}; }
  static constexpr final_awaiter final_suspend() noexcept { return {}; }
  std::suspend_always yield_value(Yielded val) noexcept {
    bottom_value() = std::addressof(val);
    return {};
  }

  auto yield_value(const yielded_deref &val) noexcept(
      std::is_nothrow_constructible_v<yielded_decvref, const yielded_deref &>)
    requires std::is_rvalue_reference_v<Yielded> &&
             std::constructible_from<yielded_decvref, const yielded_deref &>
  {
    return copy_awaiter{val, bottom_value()};
  }

  template <typename R2, typename V2>
    requires std::same_as<yield2_t<R2, V2>, Yielded>
  auto yield_value(elements_of<generator<R2, V2> &&> r) noexcept {
    return recursive_awatier{std::move(r.range)};
  }

  template <std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_reference_t<R>, Yielded>
  auto yield_value(elements_of<R> r) {
    auto n = [](std::ranges::iterator_t<R> i,
                std::ranges::sentinel_t<R> s) -> generator<Yielded, std::ranges::range_value_t<R>> {
      for (; i != s; ++i) {
        co_yield static_cast<Yielded>(*i);
      }
    };
    return yield_value(elements_of{n(std::ranges::begin(r.range), std::ranges::end(r.range))});
  }

  void unhandled_exception() {
    if (this->state_.is_bottom())
      throw;
    else
      this->except_ = std::current_exception();
  }

  void await_transform() = delete;
  void return_void() const noexcept {}

private:
  value_ptr &bottom_value() noexcept { return state_.bottom_value(); }
  value_ptr &value() noexcept { return state_.value(); }

  subyield_state state_;
  std::exception_ptr except_;
};

template <typename Yielded> struct promise_erased<Yielded>::subyield_state {
  struct frame {
    co_handle bottom_;
    co_handle parent_;
  };

  struct bottom_frame {
    co_handle top_;
    value_ptr value_ = nullptr;
  };

  std::variant<bottom_frame, frame> stack_;

  bool is_bottom() const noexcept { return std::holds_alternative<bottom_frame>(this->stack_); }

  co_handle &top() noexcept {
    if (auto f = std::get_if<frame>(&this->stack_))
      return f->bottom_.promise().state_.top();

    auto bf = std::get_if<bottom_frame>(&this->stack_);
    assert(bf && "bf is null");
    return bf->top_;
  }

  void push(co_handle current, co_handle subyield) noexcept {
    assert(&current.promise().state_ == this);
    assert(this->top() == current);

    this->top() = subyield;
    auto bc = current;
    if (auto f = std::get_if<frame>(&this->stack_))
      bc = f->bottom_;

    subyield.promise().state_.stack_ = frame{.bottom_ = bc, .parent_ = current};
  }

  std::coroutine_handle<> pop() noexcept {
    if (auto f = std::get_if<frame>(&this->stack_)) {
      auto p = this->top() = f->parent_;
      return p;
    } else
      return std::noop_coroutine();
  }

  value_ptr &bottom_value() noexcept {
    if (auto bf = std::get_if<bottom_frame>(&this->stack_))
      return bf->value_;

    auto f = std::get_if<frame>(&this->stack_);
    assert(f != nullptr);
    return f->bottom_.promise().state_.value();
  }

  value_ptr &value() noexcept {
    auto bf = std::get_if<bottom_frame>(&this->stack_);
    assert(bf != nullptr);
    return bf->value_;
  }
};

template <typename Yielded> struct promise_erased<Yielded>::copy_awaiter {
  yielded_decvref val_;
  value_ptr &bottom_val_;

  static constexpr bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<>) noexcept { this->bottom_val_ = std::addressof(val_); }
  static constexpr void await_resume() noexcept {}
};

template <typename Yielded>
template <typename Gen>
struct promise_erased<Yielded>::recursive_awatier {
  Gen gen_;

  recursive_awatier(Gen gen) noexcept : gen_(std::move(gen)) { gen_.mark_as_started(); }

  static constexpr bool await_ready() noexcept { return false; }
  template <typename Promise> co_handle await_suspend(std::coroutine_handle<Promise> p) noexcept {
    auto c = co_handle::from_address(p.address());
    auto s = co_handle::from_address(this->gen_.coro_.address());
    c.promise().state_.push(c, s);
    return s;
  }
  void await_resume() {
    if (auto e = this->gen_.coro_.promise().except_) {
      std::rethrow_exception(e);
    }
  }
};

template <typename Yielded> struct promise_erased<Yielded>::final_awaiter {
  static constexpr bool await_ready() noexcept { return false; }
  template <typename Promise>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> p) noexcept {
    return p.promise().state_.pop();
  }
  static constexpr void await_resume() noexcept {}
};

} // namespace core::gen

// TODO: add allocator
template <typename Ref, typename Val>
class generator : public std::ranges::view_interface<generator<Ref, Val>> {

  using Value = std::conditional_t<std::is_void_v<Val>, std::remove_cvref_t<Ref>, Val>;
  using Reference = reference_t<Ref, Val>;

  // TODO: Required to model indirectly_readable, and input_iterator

  using Yielded = yield_t<Reference>;
  using Erased_promise = core::gen::promise_erased<Yielded>;
  friend Erased_promise;
  struct Iterator;

public:
  generator(const generator &) = delete;

  generator(generator &&other) noexcept
      : coro_{std::exchange(other.coro_, nullptr)}, begin_{std::exchange(other.begin_, false)} {}

  ~generator() {
    if (auto &c = this->coro_)
      c.destroy();
  }

  generator &operator=(generator &&other) noexcept {
    if (&other != this) {
      std::swap(this->coro_, other.coro_);
      std::swap(this->begin_, other.begin_);
    }
    return *this;
  }

  Iterator begin() {
    this->mark_as_started();
    auto h = coro_handle::from_promise(this->coro_.promise());
    h.promise().state_.top() = h;
    return {h};
  }

  auto end() const noexcept { return std::default_sentinel; }

  struct promise_type : Erased_promise {
    generator get_return_object() noexcept {
      return {std::coroutine_handle<promise_type>::from_promise(*this)};
    }
  };

private:
  void mark_as_started() noexcept {
    assert(!begin_);
    this->begin_ = true;
  }

  using coro_handle = std::coroutine_handle<Erased_promise>;

  generator(std::coroutine_handle<promise_type> coro) noexcept : coro_{std::move(coro)} {}

  std::coroutine_handle<promise_type> coro_;
  bool begin_ = false;
};

template <typename Ref, typename Val> struct generator<Ref, Val>::Iterator {

  using value_type = Value;
  using difference_type = std::ptrdiff_t;

  friend bool operator==(const Iterator &i, std::default_sentinel_t) noexcept {
    return i.coro_.done();
  }

  Iterator &operator++() {
    this->next();
    return *this;
  }

  void operator++(int) { this->operator++(); }

  Reference operator*() const { return static_cast<Reference>(*coro_.promise().state_.value()); }

  void next() { coro_.promise().state_.top().resume(); }

private:
  friend class generator;
  Iterator(coro_handle coro) : coro_{coro} { this->next(); }

  coro_handle coro_;
};

} // namespace mira::co

#endif