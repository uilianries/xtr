// Copyright 2020 Chris E. Holloway
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "xtr/logger.hpp"

#include "xtr/detail/commands/frame.hpp"
#include "xtr/detail/commands/message_id.hpp"
#include "xtr/detail/commands/requests.hpp"
#include "xtr/detail/commands/responses.hpp"
#include "xtr/detail/config.hpp"

#include "command_client.hpp"

#include <catch2/catch.hpp>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <err.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

namespace xtrd = xtr::detail;

namespace
{
    struct test_clock
    {
        typedef std::int64_t rep;
        typedef std::ratio<1, 1000000000> period;
        typedef std::chrono::duration<rep, period> duration;
        typedef std::chrono::time_point<test_clock> time_point;

        time_point now() const noexcept
        {
            return time_point(duration(nanos_->load()));
        }

        std::time_t to_time_t(time_point tp) const noexcept
        {
            return
                std::chrono::duration_cast<std::chrono::seconds>(
                    tp.time_since_epoch()).count();
        }

        std::atomic<std::int64_t>* nanos_;
    };

    struct file_buf
    {
        file_buf()
        {
            REQUIRE(fp_ != nullptr);
        }

        ~file_buf()
        {
            std::fclose(fp_);
            std::free(buf_);
        }

        void push_lines(std::vector<std::string>& lines)
        {
            if (fp_ == nullptr)
                return;
            // Note: buf_ may be updated by stdio/open_memstream (as in, a new
            // buffer may be allocated, and the buf_ pointer overwritten to
            // point to the new buffer), hence using std::string and not
            // std::string_view in lines_.
            std::fflush(fp_);
            if (size_ == 0)
                return;
            for (
                std::size_t nl = 0;
                nl = std::strcspn(&buf_[off_], "\n"), buf_[off_ + nl] != '\0';
                off_ += nl + 1)
            {
                lines.emplace_back(&buf_[off_], nl);
            }
        }

        std::size_t size_{};
        std::size_t off_{};
        char *buf_ = nullptr;
        FILE* fp_{::open_memstream(&buf_, &size_)};
    };

    auto make_output_func(std::vector<std::string>& v, std::mutex& m)
    {
        return
            [&v, &m](xtr::log_level_t, const char* buf, std::size_t length)
            {
                std::scoped_lock lock{m};
                assert(buf[length - 1] == '\n');
                v.push_back(std::string(buf, length - 1));
                return length;
            };
    }

    auto make_error_func(std::vector<std::string>& v, std::mutex& m)
    {
        return
            [out = make_output_func(v, m)](const char* buf, std::size_t length)
            {
                out(xtr::log_level_t::none, buf, length);
            };
    }

    struct fixture
    {
        fixture() = default;

        template<typename... Args>
        fixture(Args&&... args)
        :
            log_(std::forward<Args>(args)...)
        {
        }

        virtual void sync()
        {
            s_.sync();
        }

        std::string last_line()
        {
            sync();
            std::scoped_lock lock{m_};
            REQUIRE(!lines_.empty());
            return lines_.back();
        }

        std::string last_err()
        {
            sync();
            std::scoped_lock lock{m_};
            REQUIRE(!errors_.empty());
            return errors_.back();
        }

        std::size_t line_count()
        {
            std::scoped_lock lock{m_};
            return lines_.size();
        }

        void clear_lines()
        {
            std::scoped_lock lock{m_};
            lines_.clear();
        }

        int line_{};
        std::mutex m_;
        std::vector<std::string> lines_;
        std::vector<std::string> errors_;
        std::atomic<std::int64_t> clock_nanos_{946688523123456789L};
        xtr::logger log_{
            make_output_func(lines_, m_),
            make_error_func(errors_, m_),
            test_clock{&clock_nanos_},
            xtr::null_command_path};
        xtr::sink s_ = log_.get_sink("Name");
    };

    struct file_fixture_base
    {
    protected:
        file_buf outbuf_;
        file_buf errbuf_;
    };

    struct file_fixture : file_fixture_base, fixture
    {
        file_fixture()
        :
            fixture(
                outbuf_.fp_,
                errbuf_.fp_,
                test_clock{&clock_nanos_},
                xtr::null_command_path)
        {
        }

        void sync() override
        {
            fixture::sync();
            outbuf_.push_lines(lines_);
            errbuf_.push_lines(errors_);
        }
    };

    FILE* fmktemp(char* path)
    {
        const int fd = ::mkstemp(path);
        REQUIRE(fd != -1);
        return ::fdopen(fd, "w");
    }

    struct path_fixture
    {
        ~path_fixture()
        {
            ::unlink(path_);
        }

        char path_[32] = "/tmp/xtr.test.XXXXXX";
        FILE* fp_ = fmktemp(path_);
        std::atomic<std::int64_t> clock_nanos_{946688523123456789L};
        xtr::logger log_{
            path_,
            fp_,
            fp_,
            test_clock{&clock_nanos_},
            xtr::null_command_path};
        xtr::sink s_ = log_.get_sink("Name");
    };

    template<typename Fixture = fixture>
    struct command_fixture : xtrd::command_client, Fixture
    {
        command_fixture()
        {
            const std::string& path = xtr::default_command_path();
            this->log_.set_command_path(path);
            connect(path);
        }
    };

#if __cpp_exceptions
    struct thrower {};

    std::ostream& operator<<(std::ostream& os, thrower)
    {
        throw std::runtime_error("Exception error text");
        return os;
    }
#endif

    struct custom_format
    {
        int x;
        int y;
    };

    struct streams_format
    {
        int x;
        int y;
    };

    struct non_copyable
    {
        explicit non_copyable(int x)
        :
            x_(x)
        {
        }

        non_copyable(const non_copyable&) = delete;
        non_copyable& operator=(const non_copyable&) = delete;
        non_copyable(non_copyable&&) = default;
        non_copyable& operator=(non_copyable&&) = default;

        int x_;
    };

    template<std::size_t Align>
    struct alignas(Align) align_format
    {
        int x;
    };

    std::ostream& operator<<(std::ostream& os, const streams_format& s)
    {
        return os << "(" << s.x << ", " << s.y << ")";
    }

    template<std::size_t Align>
    std::ostream& operator<<(std::ostream& os, const align_format<Align>& a)
    {
        return os << a.x;
    }

    std::ostream& operator<<(std::ostream& os, const non_copyable& n)
    {
        return os << n.x_;
    }

    struct blocker
    {
        struct data
        {
            std::mutex m_;
            std::condition_variable cv_;
            bool blocked_ = true;
        };

        void wait() const
        {
            {
                std::unique_lock lock{data_->m_};
                while (data_->blocked_)
                    data_->cv_.wait(lock);
            }
            delete data_;
        }

        void release()
        {
            std::scoped_lock lock{data_->m_};
            data_->blocked_ = false;
            data_->cv_.notify_one(); // Must be done under protection of m_
        }

        data* data_{new data};
    };

    struct capture_cerr
    {
        capture_cerr()
        {
            prev_buf_ = std::cerr.rdbuf(buf_.rdbuf());
        }

        ~capture_cerr()
        {

            std::cerr.rdbuf(prev_buf_);
        }

        std::stringstream buf_;
        std::streambuf* prev_buf_;
    };

    struct move_thrower
    {
        move_thrower() = default;
        move_thrower(move_thrower&&);
        move_thrower(const move_thrower&) noexcept;
    };

    struct copy_thrower
    {
        copy_thrower() = default;
        copy_thrower(copy_thrower&&) noexcept;
        copy_thrower(const copy_thrower&);
    };
}

namespace fmt
{
    template<>
    struct formatter<custom_format>
    {
        template<typename ParseContext>
        constexpr auto parse(ParseContext &ctx)
        {
            return ctx.begin();
        }

        template<typename FormatContext>
        auto format(const custom_format &c, FormatContext &ctx)
        {
            return format_to(ctx.out(), "({}, {})", c.x, c.y);
        }
    };

    template<>
    struct formatter<blocker>
    {
        template<typename ParseContext>
        constexpr auto parse(ParseContext &ctx)
        {
            return ctx.begin();
        }

        template<typename FormatContext>
        auto format(const blocker &b, FormatContext &ctx)
        {
            b.wait();
            return format_to(ctx.out(), "<blocker>");
        }
    };
}

using namespace fmt::literals;

TEST_CASE_METHOD(fixture, "logger no arguments test", "[logger]")
{
    XTR_LOG(s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger arithmetic types test", "[logger]")
{
    XTR_LOG(s_, "Test {}", (short)42), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));

    XTR_LOG(s_, "Test {}", (unsigned short)42), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));

    XTR_LOG(s_, "Test {}", 42), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));

    XTR_LOG(s_, "Test {}", 42U), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));

    XTR_LOG(s_, "Test {}", 42L), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));

    XTR_LOG(s_, "Test {}", 42UL), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));

    XTR_LOG(s_, "Test {}", 42LL), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));

    XTR_LOG(s_, "Test {}", 42ULL), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));

    XTR_LOG(s_, "Test {}", true), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test true"_format(line_));

    XTR_LOG(s_, "Test {:.2f}", 42.42f), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42.42"_format(line_));

    XTR_LOG(s_, "Test {}", 42.42), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42.42"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger mixed types test", "[logger]")
{
    XTR_LOG(s_, "Test {:.1f} {}", 42.0, 42), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42.0 42"_format(line_));
    XTR_LOG(s_, "Test {:.1f} {} {:.1f}", 42.0, 42, 42.0), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42.0 42 42.0"_format(line_));
    XTR_LOG(s_, "Test {:.1f} {} {:.1f} {}", 42.0, 42, 42.0, 42), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42.0 42 42.0 42"_format(line_));
}

TEST_CASE_METHOD(file_fixture, "logger file buffer test", "[logger]")
{
    XTR_LOG(s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));

    XTR_LOG(s_, "Test {}", 42), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));

    const char*s = "Hello world";
    XTR_LOG(s_, "Test {}", s), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test Hello world"_format(line_));

    const std::string_view sv{"Hello world"};
    XTR_LOG(s_, "Test {}", sv), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test Hello world"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger string copy test", "[logger]")
{
    {
        blocker b;
        const char s[] = "String 1 contents";
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", s), line_ = __LINE__;
        std::strcpy(const_cast<char*>(s), "DEADBEEF");
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test String 1 contents"_format(line_));
    }

    {
        blocker b;
        char s[] = "String 2 contents";
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", s), line_ = __LINE__;
        std::strcpy(s, "DEADBEEF");
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test String 2 contents"_format(line_));
    }

    {
        blocker b;
        char s[] = "String 3 contents";
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", std::move(s)), line_ = __LINE__;
        std::strcpy(s, "DEADBEEF");
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test String 3 contents"_format(line_));
    }

    {
        blocker b;
        char storage[] = "String 4 contents";
        const char* s = storage;
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", s), line_ = __LINE__;
        std::strcpy(storage, "DEADBEEF");
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test String 4 contents"_format(line_));
    }

    {
        blocker b;
        char storage[] = "String 5 contents";
        char* s = storage;
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", s), line_ = __LINE__;
        std::strcpy(storage, "DEADBEEF");
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test String 5 contents"_format(line_));
    }

    {
        blocker b;
        char storage[] = "String 6 contents";
        char* s = storage;
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", std::move(s)), line_ = __LINE__;
        std::strcpy(storage, "DEADBEEF");
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test String 6 contents"_format(line_));
    }

    {
        blocker b;
        char storage[] = "String 7 contentsBADCODE";
        const std::string_view s{storage, 17};
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", s), line_ = __LINE__;
        std::memcpy(storage, "DEADBEEF", 8);
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test String 7 contents"_format(line_));
    }

    {
        blocker b;
        char storage[] = "String 8 contentsBADCODE";
        std::string_view s{storage, 17};
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", s), line_ = __LINE__;
        std::memcpy(storage, "DEADBEEF", 8);
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test String 8 contents"_format(line_));
    }

    {
        blocker b;
        char storage[] = "String 9 contentsBADCODE";
        std::string_view s{storage, 17};
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", std::move(s)), line_ = __LINE__;
        std::memcpy(storage, "DEADBEEF", 8);
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test String 9 contents"_format(line_));
    }

    {
        blocker b;
        const std::string s = "String 10 contents";
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", s), line_ = __LINE__;
        std::strcpy(const_cast<char*>(&s[0]), "DEADBEEF");
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test String 10 contents"_format(line_));
    }

    {
        blocker b;
        std::string s = "String 11 contents";
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", s), line_ = __LINE__;
        std::strcpy(&s[0], "DEADBEEF");
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test String 11 contents"_format(line_));
    }
}

TEST_CASE_METHOD(fixture, "logger string move test", "[logger]")
{
    blocker b;
    std::string s = "String contents...............";
    char* storage = &s[0];
    XTR_LOG(s_, "{}", b);
    XTR_LOG(s_, "Test {}", std::move(s)), line_ = __LINE__;
    std::strcpy(storage, "Replaced contents............!");
    b.release();
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test Replaced contents............!"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger string reference test", "[logger]")
{
    {
        blocker b;
        const char s[] = "String 1 contents";
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", nocopy(s)), line_ = __LINE__;
        std::strcpy(const_cast<char*>(s), "Replaced contents");
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test Replaced contents"_format(line_));
    }

    {
        blocker b;
        char s[] = "String 2 contents";
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", nocopy(s)), line_ = __LINE__;
        std::strcpy(s, "Replaced contents");
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test Replaced contents"_format(line_));
    }

    {
        blocker b;
        char storage[] = "String 3 contents";
        const char* s = storage;
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", nocopy(s)), line_ = __LINE__;
        std::strcpy(storage, "Replaced contents");
        // Also replace the string pointed to by s, in case a
        // reference to s was incorrectly taken.
        s = "String 3 CODEBAD";
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test Replaced contents"_format(line_));
    }

    {
        blocker b;
        char storage[] = "String 4 contents";
        char* s = storage;
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", nocopy(s)), line_ = __LINE__;
        std::strcpy(storage, "Replaced contents");
        char storage2[] = "String 4 CODEBAD";
        s = storage2;
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test Replaced contents"_format(line_));
    }

    {
        blocker b;
        char storage[] = "String 5 contentsBADCODE";
        const std::string_view s{storage, 17};
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", nocopy(s)), line_ = __LINE__;
        std::memcpy(storage, "Replaced contents", 17);
        const_cast<std::string_view&>(s) = "String 5 contentsCODEBAD";
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test Replaced contents"_format(line_));
    }

    {
        blocker b;
        char storage[] = "String 6 contentsBADCODE";
        std::string_view s{storage, 17};
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", nocopy(s)), line_ = __LINE__;
        std::memcpy(storage, "Replaced contents", 17);
        s = "String 6 contentsCODEBAD";
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test Replaced contents"_format(line_));
    }

    {
        blocker b;
        const std::string s = "String 7 contents";
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", nocopy(s)), line_ = __LINE__;
        std::strcpy(const_cast<char*>(&s[0]), "Replaced contents");
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test Replaced contents"_format(line_));
    }

    {
        blocker b;
        std::string s = "String 8 contents";
        XTR_LOG(s_, "{}", b);
        XTR_LOG(s_, "Test {}", nocopy(s)), line_ = __LINE__;
        std::strcpy(&s[0], "Replaced contents");
        b.release();
        REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test Replaced contents"_format(line_));
    }
}

TEST_CASE_METHOD(fixture, "logger string table test", "[logger]")
{
    // Try to stress the string table building
    const char* s1 = "foo";
    const std::string_view s2{"barBADCODE", 3};
    const char* s3 = "baz";
    const std::string_view s4{"blepBADCODE", 4};
    const std::string_view s5{"blopBADCODE", 4};
    const char* s6 = "";
    const char* s7 = "slightly longer string";
    XTR_LOG(s_, "Test {} {} {} {} {} {} {}", s1, s2, s3, s4, s5, s6, s7), line_ = __LINE__;
    REQUIRE(
        last_line() ==
        "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: "
        "Test foo bar baz blep blop  slightly longer string"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger string overflow test", "[logger]")
{
    // Three pointers are for the formatter pointer, string pointer and record
    // size -1 is for the terminating nul on the string.
    std::string s(64UL * 1024UL - sizeof(void*) * 3 - 1, char('X'));
    XTR_LOG(s_, "Test {}", s), line_ = __LINE__;
    REQUIRE(
        last_line() ==
        "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test {}"_format(line_, s));

    s += 'Y';

    XTR_LOG(s_, "Test {}", s), line_ = __LINE__;

    REQUIRE(
        last_line() ==
        "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test <truncated>"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger const string overflow test", "[logger]")
{
    const std::string s(64UL * 1024UL - sizeof(void*) * 3, char('X'));
    XTR_LOG(s_, "Test {}", s), line_ = __LINE__;
    REQUIRE(
        last_line() ==
        "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test <truncated>"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger string_view overflow test", "[logger]")
{
    // Three pointers are for the formatter pointer, string pointer and record
    // size -1 is for the terminating nul on the string.
    std::string s(64UL * 1024UL - sizeof(void*) * 3 - 1, char('X'));
    std::string_view sv{s};
    XTR_LOG(s_, "Test {}", sv), line_ = __LINE__;
    REQUIRE(
        last_line() ==
        "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test {}"_format(line_, sv));

    s += 'Y';
    sv = std::string_view{s};

    XTR_LOG(s_, "Test {}", sv), line_ = __LINE__;

    REQUIRE(
        last_line() ==
        "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test <truncated>"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger c string overflow test", "[logger]")
{
    // Three pointers are for the formatter pointer, string pointer and record
    // size -1 is for the terminating nul on the string.
    std::string s(64UL * 1024UL - sizeof(void*) * 3 - 1, char('X'));
    XTR_LOG(s_, "Test {}", s.c_str()), line_ = __LINE__;
    REQUIRE(
        last_line() ==
        "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test {}"_format(line_, s));

    s += 'Y';

    XTR_LOG(s_, "Test {}", s.c_str()), line_ = __LINE__;

    REQUIRE(
        last_line() ==
        "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test <truncated>"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger streams formatter test", "[logger]")
{
    streams_format s{10, 20};
    XTR_LOG(s_, "Streams {}", s), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Streams (10, 20)"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger custom formatter test", "[logger]")
{
    custom_format c{10, 20};
    XTR_LOG(s_, "Custom {}", c), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Custom (10, 20)"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger timestamp test", "[logger]")
{
    clock_nanos_ = 0;
    sync();
    XTR_LOG(s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "I 1970-01-01 00:00:00.000000 Name logger.cpp:{}: Test"_format(line_));

    clock_nanos_ = 1000;
    sync();
    XTR_LOG(s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "I 1970-01-01 00:00:00.000001 Name logger.cpp:{}: Test"_format(line_));

    clock_nanos_ = 4858113906123456000;
    sync();
    XTR_LOG(s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "I 2123-12-13 04:05:06.123456 Name logger.cpp:{}: Test"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger sink arbitrary timestamp test", "[logger]")
{
    std::timespec ts;
    ts.tv_sec = 631155723;
    ts.tv_nsec = 654321000;

    xtr::timespec ts1(ts);
    ts1 = ts;

    XTR_LOG_TS(s_, ts1, "Test {}", 42), line_ = __LINE__;
    REQUIRE(last_line() == "I 1990-01-01 01:02:03.654321 Name logger.cpp:{}: Test 42"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger sink rtc timestamp test", "[logger]")
{
    // Only somewhat sane way I can think of to test XTR_LOG_RTC
    const auto ts = xtrd::get_time<XTR_CLOCK_REALTIME_FAST>();
    XTR_LOG_TS(s_, ts, "Test {}", 42), line_ = __LINE__;
    REQUIRE(last_line() == "I {} Name logger.cpp:{}: Test 42"_format(ts, line_));
}

TEST_CASE_METHOD(fixture, "logger sink tsc timestamp test", "[logger]")
{
    // Only somewhat sane way I can think of to test XTR_LOG_TSC
    const auto ts = xtrd::tsc::now();
    XTR_LOG_TS(s_, ts, "Test {}", 42), line_ = __LINE__;
    const auto logged = last_line();
    const auto expected = "I {} Name logger.cpp:{}: Test 42"_format(ts, line_);
    // The timestamps in logged and expected may be off by a small margin due
    // to tsc::to_timespec's calibration state being stored in thread_local
    // variables, which may differ between the test thread and the logger
    // thread. This could be solved by applying dependency injection to all
    // of the clock related code, IMHO doing so would unacceptably compromise
    // the code, so instead the test is just bodged below to allow a small
    // difference in the timestamps.
    const auto timestamp_to_micros =
        [](const char* str)
        {
            std::tm tm{};
            const char* pos = ::strptime(str, "I %Y-%m-%d %T", &tm);
            REQUIRE(pos != nullptr);
            REQUIRE(*pos == '.');
            ++pos;
            // note that mktime converts local time, while str will be utc, so
            // the result of this function will be off by whatever the local
            // timezone adjustment is. This does not matter for the purpose of
            // this test.
            const std::time_t secs = std::mktime(&tm);
            std::int64_t micros = 0;
            std::from_chars(pos, pos + 6, micros);
            return secs * 1000000L + micros;
        };
    INFO(logged);
    INFO(expected);
    REQUIRE(
        std::abs(
            timestamp_to_micros(logged.c_str()) -
            timestamp_to_micros(expected.c_str())) < 100);
}

TEST_CASE_METHOD(fixture, "tsc estimation test", "[logger]")
{
    // If the tsc hz can be queried, verify that the estimated hz is close
    if (const auto hz = xtrd::read_tsc_hz())
    {
        const auto estimated_hz = xtrd::estimate_tsc_hz();
        REQUIRE(estimated_hz == Approx(hz));
    }
}

#if __cpp_exceptions
TEST_CASE_METHOD(fixture, "logger error handling test", "[logger]")
{
    XTR_LOG(s_, "Test {}", thrower{}), line_ = __LINE__;
    REQUIRE(last_err() == "E 2000-01-01 01:02:03.123456 Name: Error: Exception error text");
    REQUIRE(lines_.empty());
}
#endif

TEST_CASE_METHOD(fixture, "logger argument destruction test", "[logger]")
{
    std::weak_ptr<int> w;
    {
        auto p{std::make_shared<int>(42)};
        w = p;
        XTR_LOG(s_, "Test {}", p), line_ = __LINE__;
        sync();
        REQUIRE(!lines_.empty());
    }
    REQUIRE(w.expired());
}

TEST_CASE("logger set output file test", "[logger]")
{
    file_buf buf;
    fixture f;

    f.log_.set_output_stream(buf.fp_);

    XTR_LOG(f.s_, "Test"), f.line_ = __LINE__;

    f.sync();
    REQUIRE(f.lines_.empty());
    REQUIRE(f.errors_.empty());

    std::vector<std::string> lines;
    buf.push_lines(lines);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines.back() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(f.line_));
}

TEST_CASE("logger set error file test", "[logger]")
{
    file_buf buf;
    fixture f;

    f.log_.set_error_stream(buf.fp_);
    f.log_.set_output_function(
        [](xtr::log_level_t, const char*, std::size_t)
        {
            return -1;
        });

    XTR_LOG(f.s_, "Test"), f.line_ = __LINE__;

    f.sync();
    REQUIRE(f.lines_.empty());
    REQUIRE(f.errors_.empty());

    std::vector<std::string> lines;
    buf.push_lines(lines);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines.back() == "E 2000-01-01 01:02:03.123456 Name: Error: Write error");
}

TEST_CASE_METHOD(fixture, "logger set output func test", "[logger]")
{
    std::string output;
    xtr::log_level_t level;

    log_.set_output_function(
        [&](xtr::log_level_t l, const char* buf, std::size_t size)
        {
            output = std::string(buf, size);
            level = l;
            return size;
        });

    XTR_LOG(s_, "Test"), line_ = __LINE__;

    sync();
    REQUIRE(lines_.empty());
    REQUIRE(errors_.empty());

    REQUIRE(output == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test\n"_format(line_));
    REQUIRE(level == xtr::log_level_t::info);
}

TEST_CASE_METHOD(fixture, "logger set error func test", "[logger]")
{
    std::string error;
    log_.set_output_function(
        [](xtr::log_level_t, const char*, std::size_t)
        {
            return -1;
        });
    log_.set_error_function(
        [&error](const char* buf, std::size_t size)
        {
            error = std::string(buf, size);
        });

    XTR_LOG(s_, "Test"), line_ = __LINE__;

    sync();
    REQUIRE(lines_.empty());
    REQUIRE(errors_.empty());

    REQUIRE(error == "E 2000-01-01 01:02:03.123456 Name: Error: Write error\n");
}

TEST_CASE("logger set close func test", "[logger]")
{
    bool closed = false;

    {
        fixture f;
        f.log_.set_close_function([&closed](){ closed = true; });
        REQUIRE(!closed);
    }

    REQUIRE(closed);
}

TEST_CASE_METHOD(fixture, "logger short write test", "[logger]")
{
    log_.set_output_function(
        [&](xtr::log_level_t, const char* buf, std::size_t size)
        {
            size -= 21;
            std::scoped_lock lock{m_};
            lines_.push_back(std::string(buf, size));
            return size;
        });

    XTR_LOG(s_, "Test {} {} {}", 1, 2, 3), line_ = __LINE__;

    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger."_format(line_));
    REQUIRE(last_err() == "E 2000-01-01 01:02:03.123456 Name: Error: Short write"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger sink change name test", "[logger]")
{
    XTR_LOG(s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));

    s_.set_name("A new name");
    XTR_LOG(s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 A new name logger.cpp:{}: Test"_format(line_));

    s_.set_name("An even newer name");
    XTR_LOG(s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 An even newer name logger.cpp:{}: Test"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger no macro test", "[logger]")
{
    static constexpr xtrd::string test1{"{}{} {} Test\n"};
    s_.log<&test1, xtr::log_level_t::info>();
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name Test"_format(line_));

    static constexpr xtrd::string test2{"{}{} {} Test {}\n"};
    s_.log<&test2, xtr::log_level_t::info>(42);
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name Test 42"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger alignment test", "[logger]")
{
    XTR_LOG(s_, "Test {}", align_format<4>{42}), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));

    XTR_LOG(s_, "Test {}", align_format<8>{42}), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));

    XTR_LOG(s_, "Test {}", align_format<16>{42}), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));

    XTR_LOG(s_, "Test {}", align_format<32>{42}), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));

    XTR_LOG(s_, "Test {}", align_format<64>{42}), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger argument move test", "[logger]")
{
    non_copyable nc(42);
    XTR_LOG(s_, "Test {}", std::move(nc)), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test 42"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger non-blocking test", "[logger]")
{
    XTR_TRY_LOG(s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger non-blocking drop test", "[logger]")
{
    // 64kb default size buffer, 8 bytes per log record, 16 bytes taken
    // by blocker.
    const std::size_t n_dropped = 100;
    const std::size_t blocker_sz = 16;
    const std::size_t msg_sz = 8;
    const std::size_t n = (64 * 1024 - blocker_sz) / msg_sz + n_dropped;

    auto next_sink = log_.get_sink("next");

    blocker b;

    XTR_LOG(s_, "{}", b);

    for (std::size_t i = 0; i < n; ++i)
        XTR_TRY_LOG(s_, "Test");

    b.release();
    sync();

    // At this point it is not guaranteed that the dropped count has been
    // printed, because the consumer thread could have read the sync request
    // in the same 'batch' of messages as the log requests. Instead sync must
    // be called on the next sink, which ensures that the consumer has
    // processed the dropped count of s_, as it must do so before moving on to
    // the next sink.
    next_sink.sync();

    REQUIRE(last_line() == "W 2000-01-01 01:02:03.123456 Name: {} messages dropped"_format(n_dropped));
}

// Calling these tests `soak' tests is stretching things but I can't think
// of a better name.

TEST_CASE_METHOD(fixture, "logger soak test", "[logger]")
{
    constexpr std::size_t n = 100000;

    for (std::size_t i = 0; i < n; ++i)
    {
        XTR_LOG(s_, "Test {}", i), line_ = __LINE__;
        XTR_LOG(s_, "Test {}", i);
    }

    sync();
    REQUIRE(line_count() == n * 2);

    for (std::size_t i = 0; i < n * 2; i += 2)
    {
        REQUIRE(lines_[i] == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test {}"_format(line_, i / 2));
        REQUIRE(lines_[i + 1] == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test {}"_format(line_ + 1, i / 2));
    }
}

TEST_CASE_METHOD(fixture, "logger multiple sink soak test", "[logger]")
{
    constexpr std::size_t n = 100000;
    constexpr std::size_t n_sinks = 1024;

    std::vector<xtr::sink> sinks;

    for (std::size_t i = 0; i < n_sinks; ++i)
        sinks.push_back(log_.get_sink("Name"));

    for (std::size_t i = 0; i < n; ++i)
    {
        auto& p = sinks[i & (n_sinks - 1)];
        XTR_LOG(p, "Test"), line_ = __LINE__;
    }

    for (std::size_t i = 0; i < n; ++i)
    {
        auto& p = sinks[i & (n_sinks - 1)];
        p.sync();
    }

    REQUIRE(line_count() == n);

    for (std::size_t i = 0; i < n; ++i)
        REQUIRE(lines_[i] == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger string soak test", "[logger]")
{
    constexpr std::size_t n = 100000;
    const char* s = "A string to be copied into the logger buffer";

    for (std::size_t i = 0; i < n; ++i)
    {
        XTR_LOG(s_, "Test {}", s), line_ = __LINE__;
        XTR_LOG(s_, "Test {}", s);
    }

    sync();
    REQUIRE(line_count() == n * 2);

    for (std::size_t i = 0; i < n * 2; i += 2)
    {
        REQUIRE(lines_[i] == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test {}"_format(line_, s));
        REQUIRE(lines_[i + 1] == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test {}"_format(line_ + 1, s));
    }
}

TEST_CASE_METHOD(fixture, "logger unprintable characters test", "[logger]")
{
    const char *s = "\nTest\r\nTest";
    XTR_LOG(s_, "{}", s), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: \\x0ATest\\x0D\\x0ATest"_format(line_));

    XTR_LOG(s_, "{}", nocopy(s)), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: \\x0ATest\\x0D\\x0ATest"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger escape sequence test", "[logger]")
{
    const char* s = "\x1b]0;Test\x07";
    XTR_LOG(s_, "{}", s), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: \\x1B]0;Test\\x07"_format(line_));

    XTR_LOG(s_, "{}", nocopy(s)), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: \\x1B]0;Test\\x07"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger sync test", "[logger]")
{
    std::atomic<std::size_t> sync_count{};

    log_.set_sync_function(
        [&sync_count]()
        {
            ++sync_count;
        });

    // set_sync_function calls sync
    REQUIRE(sync_count == 1);

    const std::size_t n = 10;

    for (std::size_t i = 0; i < n; ++i)
    {
        std::size_t target_count = sync_count + 1;
        sync();
        REQUIRE(sync_count == target_count);
    }

    // Set an empty function to avoid accessing a dangling reference
    // to sync_count
    log_.set_sync_function([](){});
}

TEST_CASE_METHOD(fixture, "logger flush test", "[logger]")
{
    std::atomic<std::size_t> flush_count{};

    log_.set_flush_function(
        [&flush_count]()
        {
            ++flush_count;
        });

    // set_flush_function calls sync, which calls flush, so here flush_count
    // is either 1, or 2 if flush was called in between set_flush_function
    // and sync being processed.
    REQUIRE((flush_count == 1 || flush_count == 2));

    const std::size_t n = 10;

    for (std::size_t i = 0; i < n; ++i)
    {
        std::size_t target_count = flush_count + 1;
        XTR_LOG(s_, "Test");
        // Wait for flush to be called, unfortunately this is racy, however
        // sync() cannot be called as it would interfere with the test itself. 
        for (std::size_t j = 0; j < 10000 && flush_count < target_count; ++j)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(flush_count == target_count);
    }

    // Set an empty function to avoid accessing a dangling reference
    // to flush_count
    log_.set_flush_function([](){});
}

TEST_CASE_METHOD(path_fixture, "logger throughput", "[.logger]")
{
    using clock = std::chrono::high_resolution_clock;

    constexpr std::size_t n = 10000000;

    static constexpr char fmt[] =
        "{}{} Test message of length 80 chars Test message of length 80 chars Test message of\n";

    const auto print_result =
        [](auto t0, auto t1, const char* name)
        {
            const std::chrono::duration<double> delta = t1 - t0;
            const double messages_sec = n / delta.count();
            // 33 is # of chars in timestamp and name
            const double bytes_per_message = double(sizeof(fmt) - 1 + 33);
            const double mb_sec = (messages_sec * bytes_per_message) / (1024 * 1024);
            std::cout
                << name << " messages/s: " << std::size_t(messages_sec) << ", "
                << "MB/s: " << mb_sec << "\n";
        };

    {
        const auto t0 = clock::now();
        for (std::size_t i = 0; i < n; ++i)
            s_.log<&fmt, xtr::log_level_t::info>();
        s_.sync();
        const auto t1 = clock::now();
        print_result(t0, t1, "Logger");
    }

    {
        // This isn't printing exactly what the logger does, but it's close enough
        const auto t0 = clock::now();
        for (std::size_t i = 0; i < n; ++i)
            fmt::print(fp_, fmt, "2019-10-17 20:59:03: Name   ", i);
        s_.sync();
        const auto t1 = clock::now();
        print_result(t0, t1, "Fmt");
    }
}

TEST_CASE("logger no fixture test", "[.logger]")
{
    xtr::logger log;
    auto p = log.get_sink("Test");
    XTR_LOG(p, "Hello world");
    XTR_LOG(p, "Hello world {}", 42);
    p.set_level(xtr::log_level_t::debug);
    XTR_LOGL(error, p, "Hello {}", "amazing");
    XTR_LOGL(warning, p, "Hello {}", "amazing");
    XTR_LOGL(info, p, "Hello {}", "amazing");
    XTR_LOGL(debug, p, "Hello {}", "amazing");
}

TEST_CASE("logger no exit test", "[.logger]")
{
    // For manually testing log rotation via the reopen command

    xtr::logger log{"/tmp/testlog"};

    std::vector<xtr::sink> sinks;

    for (std::size_t i = 0; i < 5; ++i)
        sinks.push_back(log.get_sink("Test" + std::to_string(i)));

    XTR_LOG(sinks[0], "Hello world");

    for (std::size_t i = 0; ; ++i)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        XTR_LOG(sinks[i % 5], "Hello world");
    }
}

TEST_CASE_METHOD(fixture, "logger sink copy test", "[logger]")
{
    xtr::sink p_copy(s_);
    p_copy.set_name("p_copy");
    xtr::sink p_assign;
    p_assign = p_copy;
    p_assign.set_name("p_assign");

    std::vector<xtr::sink> v;

    v.push_back(p_assign);
    v.back().set_name("vec0");
    v.push_back(p_assign);
    v.back().set_name("vec1");
    v.push_back(p_assign);
    v.back().set_name("vec2");

    XTR_LOG(p_copy, "Test"), line_ = __LINE__;
    p_copy.sync();
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 p_copy logger.cpp:{}: Test"_format(line_));

    XTR_LOG(p_assign, "Test"), line_ = __LINE__;
    p_assign.sync();
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 p_assign logger.cpp:{}: Test"_format(line_));

    XTR_LOG(v[0], "Test"), line_ = __LINE__;
    v[0].sync();
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 vec0 logger.cpp:{}: Test"_format(line_));

    XTR_LOG(v[1], "Test"), line_ = __LINE__;
    v[1].sync();
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 vec1 logger.cpp:{}: Test"_format(line_));

    XTR_LOG(v[2], "Test"), line_ = __LINE__;
    v[2].sync();
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 vec2 logger.cpp:{}: Test"_format(line_));

    p_assign = p_copy;
    p_assign.set_name("p_assign2");
    XTR_LOG(p_assign, "Test"), line_ = __LINE__;
    p_assign.sync();
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 p_assign2 logger.cpp:{}: Test"_format(line_));

    p_assign.close();
    p_assign = p_copy;
    p_assign.set_name("p_assign3");
    XTR_LOG(p_assign, "Test"), line_ = __LINE__;
    p_assign.sync();
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 p_assign3 logger.cpp:{}: Test"_format(line_));

    // Copy and assign without calling set_name to verify names are
    // copied correctly.
    xtr::sink p_copy2(p_copy);
    XTR_LOG(p_copy2, "Test"), line_ = __LINE__;
    p_copy2.sync();
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 p_copy logger.cpp:{}: Test"_format(line_));

    xtr::sink p_assign2;
    p_assign2 = p_assign;
    XTR_LOG(p_assign2, "Test"), line_ = __LINE__;
    p_assign2.sync();
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 p_assign3 logger.cpp:{}: Test"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger log level test", "[logger]")
{
    s_.set_level(xtr::log_level_t::none);

    XTR_LOGL(error, s_, "Test"), line_ = __LINE__;
    XTR_LOGL(warning, s_, "Test"), line_ = __LINE__;
    XTR_LOGL(info, s_, "Test"), line_ = __LINE__;
    XTR_LOGL(debug, s_, "Test"), line_ = __LINE__;

    // At `none', nothing should be logged
    s_.sync();
    REQUIRE(lines_.empty());

    s_.set_level(xtr::log_level_t::fatal);

    XTR_LOGL(error, s_, "Test"), line_ = __LINE__;
    XTR_LOGL(warning, s_, "Test"), line_ = __LINE__;
    XTR_LOGL(info, s_, "Test"), line_ = __LINE__;
    XTR_LOGL(debug, s_, "Test"), line_ = __LINE__;

    // At `fatal', nothing should be logged (except fatal logs, however
    // fatal logs call abort() so isn't called here).
    s_.sync();
    REQUIRE(lines_.empty());

    // Error
    s_.set_level(xtr::log_level_t::error);

    XTR_LOGL(error, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "E 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
    XTR_LOGL(warning, s_, "Test"), line_ = __LINE__;
    XTR_LOGL(info, s_, "Test"), line_ = __LINE__;
    XTR_LOGL(debug, s_, "Test"), line_ = __LINE__;

    // Warning
    s_.set_level(xtr::log_level_t::warning);

    XTR_LOGL(error, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "E 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
    XTR_LOGL(warning, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "W 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
    XTR_LOGL(info, s_, "Test"), line_ = __LINE__;
    XTR_LOGL(debug, s_, "Test"), line_ = __LINE__;

    // Info
    s_.set_level(xtr::log_level_t::info);

    XTR_LOGL(error, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "E 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
    XTR_LOGL(warning, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "W 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
    XTR_LOGL(info, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
    XTR_LOGL(debug, s_, "Test"), line_ = __LINE__;

    // Debug
    s_.set_level(xtr::log_level_t::debug);

    XTR_LOGL(error, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "E 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
    XTR_LOGL(warning, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "W 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
    XTR_LOGL(info, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
    XTR_LOGL(debug, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "D 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));

    // Check total lines to ensure nothing extra was logged
    REQUIRE(lines_.size() == 10);
}

TEST_CASE_METHOD(command_fixture<>, "logger status command test", "[logger]")
{
    auto p0 = log_.get_sink("Producer0");
    auto p1 = log_.get_sink("Producer1");
    auto p2 = log_.get_sink("Producer2");
    auto p3 = log_.get_sink("Producer3");

    p0.sync();
    p1.sync();
    p2.sync();
    p3.sync();

    p0.set_level(xtr::log_level_t::debug);
    p1.set_level(xtr::log_level_t::warning);
    p2.set_level(xtr::log_level_t::error);
    p3.set_level(xtr::log_level_t::fatal);

    xtrd::frame<xtrd::status> st;

    st->pattern.type = xtrd::pattern_type_t::none;

    const auto infos = send_frame<xtrd::sink_info>(st);

    using namespace std::literals::string_view_literals;

    REQUIRE(infos.size() == 5);

    REQUIRE(infos[0].name == "Name"sv);
    REQUIRE(infos[0].level == xtr::log_level_t::info);
    REQUIRE(infos[0].buf_capacity == 64 * 1024);
    REQUIRE(infos[0].buf_nbytes == 0);
    REQUIRE(infos[0].dropped_count == 0);

    REQUIRE(infos[1].name == "Producer0"sv);
    REQUIRE(infos[1].level == xtr::log_level_t::debug);
    REQUIRE(infos[1].buf_capacity == 64 * 1024);
    REQUIRE(infos[1].buf_nbytes == 0);
    REQUIRE(infos[1].dropped_count == 0);

    REQUIRE(infos[2].name == "Producer1"sv);
    REQUIRE(infos[2].level == xtr::log_level_t::warning);
    REQUIRE(infos[2].buf_capacity == 64 * 1024);
    REQUIRE(infos[2].buf_nbytes == 0);
    REQUIRE(infos[2].dropped_count == 0);

    REQUIRE(infos[3].name == "Producer2"sv);
    REQUIRE(infos[3].level == xtr::log_level_t::error);
    REQUIRE(infos[3].buf_capacity == 64 * 1024);
    REQUIRE(infos[3].buf_nbytes == 0);
    REQUIRE(infos[3].dropped_count == 0);

    REQUIRE(infos[4].name == "Producer3"sv);
    REQUIRE(infos[4].level == xtr::log_level_t::fatal);
    REQUIRE(infos[4].buf_capacity == 64 * 1024);
    REQUIRE(infos[4].buf_nbytes == 0);
    REQUIRE(infos[4].dropped_count == 0);
}

TEST_CASE_METHOD(command_fixture<>, "logger status command dropped count test", "[logger]")
{
    // 64kb default size buffer, 8 bytes per log record, 16 bytes taken
    // by blocker.
    const std::size_t n_dropped = 100;
    const std::size_t blocker_sz = 16;
    const std::size_t msg_sz = 8;
    const std::size_t n = (64 * 1024 - blocker_sz) / msg_sz + n_dropped;

    blocker b;

    XTR_LOG(s_, "{}", b);

    for (std::size_t i = 0; i < n; ++i)
        XTR_TRY_LOG(s_, "Test");

    b.release();
    sync();

    xtrd::frame<xtrd::status> st;

    st->pattern.type = xtrd::pattern_type_t::none;

    const auto infos = send_frame<xtrd::sink_info>(st);

    using namespace std::literals::string_view_literals;

    REQUIRE(infos.size() == 1);

    REQUIRE(infos[0].name == "Name"sv);
    REQUIRE(infos[0].dropped_count == n_dropped);
}

TEST_CASE_METHOD(command_fixture<>, "logger set_level command test", "[logger]")
{
    xtrd::frame<xtrd::set_level> sl;

    sl->level = xtr::log_level_t::debug;
    sl->pattern.type = xtrd::pattern_type_t::none;

    REQUIRE(s_.level() == xtr::log_level_t::info);

    send_frame<xtrd::success>(sl);

    reconnect();

    xtrd::frame<xtrd::status> st;

    const auto infos = send_frame<xtrd::sink_info>(st);

    REQUIRE(s_.level() == xtr::log_level_t::debug);

    using namespace std::literals::string_view_literals;

    REQUIRE(infos.size() == 1);

    REQUIRE(infos[0].name == "Name"sv);
    REQUIRE(infos[0].level == xtr::log_level_t::debug);
}

TEST_CASE_METHOD(command_fixture<>, "logger set_level invalid command test", "[logger]")
{
    xtrd::frame<xtrd::set_level> sl;

    sl->level = static_cast<xtr::log_level_t>(42);
    sl->pattern.type = xtrd::pattern_type_t::none;

    const auto errors = send_frame<xtrd::error>(sl);

    using namespace std::literals::string_view_literals;

    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].reason == "Invalid level"sv);
}

TEST_CASE_METHOD(command_fixture<>, "logger status command regex test", "[logger]")
{
    auto p0 = log_.get_sink("Foo");
    auto p1 = log_.get_sink("FobFar");
    auto p2 = log_.get_sink("FooBar");
    auto p3 = log_.get_sink("Baz");

    p0.sync();
    p1.sync();
    p2.sync();
    p3.sync();

    xtrd::frame<xtrd::status> st;

    st->pattern.type = xtrd::pattern_type_t::basic_regex;
    std::strcpy(st->pattern.text, "Foo.*");

    const auto infos = send_frame<xtrd::sink_info>(st);

    using namespace std::literals::string_view_literals;

    REQUIRE(infos.size() == 2);

    REQUIRE(infos[0].name == "Foo"sv);
    REQUIRE(infos[1].name == "FooBar"sv);
}

TEST_CASE_METHOD(command_fixture<>, "logger status command invalid regex test", "[logger]")
{
    xtrd::frame<xtrd::status> st;

    st->pattern.type = xtrd::pattern_type_t::basic_regex;
    std::strcpy(st->pattern.text, "***");

    const auto errors = send_frame<xtrd::error>(st);

    using namespace std::literals::string_view_literals;

    REQUIRE(errors.size() == 1);
    REQUIRE_THAT(
        errors[0].reason,
        Catch::Matchers::Contains("invalid", Catch::CaseSensitive::No));
}

TEST_CASE_METHOD(command_fixture<>, "logger status command regex case test", "[logger]")
{
    auto p0 = log_.get_sink("Foo");
    auto p1 = log_.get_sink("foo");

    p0.sync();
    p1.sync();

    xtrd::frame<xtrd::status> st;

    st->pattern.type = xtrd::pattern_type_t::basic_regex;
    st->pattern.ignore_case = true;
    std::strcpy(st->pattern.text, "foo");

    auto infos = send_frame<xtrd::sink_info>(st);

    using namespace std::literals::string_view_literals;

    REQUIRE(infos.size() == 2);

    REQUIRE(infos[0].name == "Foo"sv);
    REQUIRE(infos[1].name == "foo"sv);

    reconnect();

    st->pattern.ignore_case = false;

    infos = send_frame<xtrd::sink_info>(st);

    REQUIRE(infos.size() == 1);

    REQUIRE(infos[0].name == "foo"sv);
}

TEST_CASE_METHOD(command_fixture<>, "logger status command wildcard test", "[logger]")
{
    auto p0 = log_.get_sink("Foo");
    auto p1 = log_.get_sink("FobFar");
    auto p2 = log_.get_sink("FooBar");
    auto p3 = log_.get_sink("Baz");

    p0.sync();
    p1.sync();
    p2.sync();
    p3.sync();

    xtrd::frame<xtrd::status> st;

    st->pattern.type = xtrd::pattern_type_t::wildcard;
    std::strcpy(st->pattern.text, "Foo*");

    const auto infos = send_frame<xtrd::sink_info>(st);

    using namespace std::literals::string_view_literals;

    REQUIRE(infos.size() == 2);

    REQUIRE(infos[0].name == "Foo"sv);
    REQUIRE(infos[1].name == "FooBar"sv);
}

TEST_CASE_METHOD(command_fixture<>, "logger status command wildcard case test", "[logger]")
{
    auto p0 = log_.get_sink("Foo");
    auto p1 = log_.get_sink("foo");

    p0.sync();
    p1.sync();

    xtrd::frame<xtrd::status> st;

    st->pattern.type = xtrd::pattern_type_t::wildcard;
    st->pattern.ignore_case = true;
    std::strcpy(st->pattern.text, "foo");

    auto infos = send_frame<xtrd::sink_info>(st);

    using namespace std::literals::string_view_literals;

    REQUIRE(infos.size() == 2);

    REQUIRE(infos[0].name == "Foo"sv);
    REQUIRE(infos[1].name == "foo"sv);

    reconnect();

    st->pattern.ignore_case = false;

    infos = send_frame<xtrd::sink_info>(st);

    REQUIRE(infos.size() == 1);

    REQUIRE(infos[0].name == "foo"sv);
}

TEST_CASE_METHOD(command_fixture<>, "logger status command extended regex test", "[logger]")
{
    auto p = log_.get_sink("foo");

    p.sync();

    xtrd::frame<xtrd::status> st;

    st->pattern.type = xtrd::pattern_type_t::extended_regex;
    std::strcpy(st->pattern.text, "fo+");

    auto infos = send_frame<xtrd::sink_info>(st);

    using namespace std::literals::string_view_literals;

    REQUIRE(infos.size() == 1);
    REQUIRE(infos[0].name == "foo"sv);
}

TEST_CASE_METHOD(command_fixture<>, "logger set_level command regex test", "[logger]")
{
    auto p0 = log_.get_sink("Foo");
    auto p1 = log_.get_sink("FobFar");
    auto p2 = log_.get_sink("FooBar");
    auto p3 = log_.get_sink("Baz");

    p0.sync();
    p1.sync();
    p2.sync();
    p3.sync();

    xtrd::frame<xtrd::set_level> sl;

    sl->level = xtr::log_level_t::debug;
    sl->pattern.type = xtrd::pattern_type_t::basic_regex;
    std::strcpy(sl->pattern.text, "Foo.*");

    send_frame<xtrd::success>(sl);

    reconnect();

    xtrd::frame<xtrd::status> st;

    st->pattern.type = xtrd::pattern_type_t::none;

    const auto infos = send_frame<xtrd::sink_info>(st);

    using namespace std::literals::string_view_literals;

    REQUIRE(infos.size() == 5);

    REQUIRE(infos[0].name == "Name"sv);
    REQUIRE(infos[0].level == xtr::log_level_t::info);
    REQUIRE(infos[1].name == "Foo"sv);
    REQUIRE(infos[1].level == xtr::log_level_t::debug);
    REQUIRE(infos[2].name == "FobFar"sv);
    REQUIRE(infos[2].level == xtr::log_level_t::info);
    REQUIRE(infos[3].name == "FooBar"sv);
    REQUIRE(infos[3].level == xtr::log_level_t::debug);
    REQUIRE(infos[4].name == "Baz"sv);
    REQUIRE(infos[4].level == xtr::log_level_t::info);
}

TEST_CASE_METHOD(command_fixture<>, "logger set_level command regex case test", "[logger]")
{
    auto p0 = log_.get_sink("Foo");
    auto p1 = log_.get_sink("foo");

    p0.sync();
    p1.sync();

    xtrd::frame<xtrd::set_level> sl;

    sl->level = xtr::log_level_t::debug;
    sl->pattern.type = xtrd::pattern_type_t::basic_regex;
    sl->pattern.ignore_case = true;
    std::strcpy(sl->pattern.text, "foo");

    send_frame<xtrd::success>(sl);

    reconnect();

    reconnect();

    xtrd::frame<xtrd::status> st;

    st->pattern.type = xtrd::pattern_type_t::none;

    auto infos = send_frame<xtrd::sink_info>(st);

    using namespace std::literals::string_view_literals;

    REQUIRE(infos.size() == 3);

    REQUIRE(infos[0].name == "Name"sv);
    REQUIRE(infos[0].level == xtr::log_level_t::info);
    REQUIRE(infos[1].name == "Foo"sv);
    REQUIRE(infos[1].level == xtr::log_level_t::debug);
    REQUIRE(infos[2].name == "foo"sv);
    REQUIRE(infos[2].level == xtr::log_level_t::debug);

    reconnect();

    sl->level = xtr::log_level_t::error;
    sl->pattern.ignore_case = false;

    send_frame<xtrd::success>(sl);

    reconnect();

    infos = send_frame<xtrd::sink_info>(st);

    REQUIRE(infos.size() == 3);

    REQUIRE(infos[0].name == "Name"sv);
    REQUIRE(infos[0].level == xtr::log_level_t::info);
    REQUIRE(infos[1].name == "Foo"sv);
    REQUIRE(infos[1].level == xtr::log_level_t::debug);
    REQUIRE(infos[2].name == "foo"sv);
    REQUIRE(infos[2].level == xtr::log_level_t::error);
}

TEST_CASE_METHOD(command_fixture<>, "logger set_level command invalid regex test", "[logger]")
{
    xtrd::frame<xtrd::set_level> sl;

    sl->level = xtr::log_level_t::debug;
    sl->pattern.type = xtrd::pattern_type_t::basic_regex;
    std::strcpy(sl->pattern.text, "***");

    const auto errors = send_frame<xtrd::error>(sl);

    using namespace std::literals::string_view_literals;

    REQUIRE(errors.size() == 1);
    REQUIRE_THAT(
        errors[0].reason,
        Catch::Matchers::Contains("invalid", Catch::CaseSensitive::No));
}

TEST_CASE_METHOD(command_fixture<>, "logger set_level command wildcard test", "[logger]")
{
    auto p0 = log_.get_sink("Foo");
    auto p1 = log_.get_sink("FobFar");
    auto p2 = log_.get_sink("FooBar");
    auto p3 = log_.get_sink("Baz");

    p0.sync();
    p1.sync();
    p2.sync();
    p3.sync();

    xtrd::frame<xtrd::set_level> sl;

    sl->level = xtr::log_level_t::debug;
    sl->pattern.type = xtrd::pattern_type_t::wildcard;
    std::strcpy(sl->pattern.text, "Foo*");

    send_frame<xtrd::success>(sl);

    reconnect();

    xtrd::frame<xtrd::status> st;

    st->pattern.type = xtrd::pattern_type_t::none;

    const auto infos = send_frame<xtrd::sink_info>(st);

    using namespace std::literals::string_view_literals;

    REQUIRE(infos.size() == 5);

    REQUIRE(infos[0].name == "Name"sv);
    REQUIRE(infos[0].level == xtr::log_level_t::info);
    REQUIRE(infos[1].name == "Foo"sv);
    REQUIRE(infos[1].level == xtr::log_level_t::debug);
    REQUIRE(infos[2].name == "FobFar"sv);
    REQUIRE(infos[2].level == xtr::log_level_t::info);
    REQUIRE(infos[3].name == "FooBar"sv);
    REQUIRE(infos[3].level == xtr::log_level_t::debug);
    REQUIRE(infos[4].name == "Baz"sv);
    REQUIRE(infos[4].level == xtr::log_level_t::info);
}

TEST_CASE_METHOD(command_fixture<>, "logger set_level command wildcard case test", "[logger]")
{
    auto p0 = log_.get_sink("Foo");
    auto p1 = log_.get_sink("foo");

    p0.sync();
    p1.sync();

    xtrd::frame<xtrd::set_level> sl;

    sl->level = xtr::log_level_t::debug;
    sl->pattern.type = xtrd::pattern_type_t::wildcard;
    sl->pattern.ignore_case = true;
    std::strcpy(sl->pattern.text, "foo");

    send_frame<xtrd::success>(sl);

    reconnect();

    xtrd::frame<xtrd::status> st;

    st->pattern.type = xtrd::pattern_type_t::none;

    auto infos = send_frame<xtrd::sink_info>(st);

    using namespace std::literals::string_view_literals;

    REQUIRE(infos.size() == 3);

    REQUIRE(infos[0].name == "Name"sv);
    REQUIRE(infos[0].level == xtr::log_level_t::info);
    REQUIRE(infos[1].name == "Foo"sv);
    REQUIRE(infos[1].level == xtr::log_level_t::debug);
    REQUIRE(infos[2].name == "foo"sv);
    REQUIRE(infos[2].level == xtr::log_level_t::debug);

    reconnect();

    sl->level = xtr::log_level_t::error;
    sl->pattern.ignore_case = false;

    send_frame<xtrd::success>(sl);

    reconnect();

    infos = send_frame<xtrd::sink_info>(st);

    REQUIRE(infos.size() == 3);

    REQUIRE(infos[0].name == "Name"sv);
    REQUIRE(infos[0].level == xtr::log_level_t::info);
    REQUIRE(infos[1].name == "Foo"sv);
    REQUIRE(infos[1].level == xtr::log_level_t::debug);
    REQUIRE(infos[2].name == "foo"sv);
    REQUIRE(infos[2].level == xtr::log_level_t::error);
}

TEST_CASE_METHOD(command_fixture<>, "logger command multiple clients test", "[logger]")
{
    std::array<xtrd::command_client, 64> clients;
    std::vector<xtr::sink> sinks;

    for (std::size_t i = 0; i < 1024; ++i)
        sinks.push_back(log_.get_sink("P" + std::to_string(i)));

    for (auto& c : clients)
        c.connect(cmd_path_);

    xtrd::frame<xtrd::status> st;
    st->pattern.type = xtrd::pattern_type_t::none;

    for (auto& c : clients)
        c.send_frame(st);

    for (auto& c : clients)
    {
        const auto infos = c.read_reply<xtrd::sink_info>();
        REQUIRE(infos.size() == 1025);
    }
}

TEST_CASE_METHOD(command_fixture<>, "logger reopen command test", "[logger]")
{
    bool reopened = false;

    log_.set_reopen_function([&reopened](){ reopened = true; return true; });

    REQUIRE(!reopened);

    xtrd::frame<xtrd::reopen> ro;
    send_frame<xtrd::success>(ro);

    REQUIRE(reopened);
}

TEST_CASE_METHOD(command_fixture<>, "logger reopen command error test", "[logger]")
{
    log_.set_reopen_function([](){ errno = EBADF; return false; });

    xtrd::frame<xtrd::reopen> ro;
    const auto errors = send_frame<xtrd::error>(ro);

    using namespace std::literals::string_view_literals;

    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].reason == "Bad file descriptor"sv);
}

TEST_CASE_METHOD(command_fixture<path_fixture>, "logger reopen command path test", "[logger]")
{
    ::unlink(path_);

    struct stat sb;

    REQUIRE(::stat(path_, &sb) == -1);

    xtrd::frame<xtrd::reopen> ro;
    send_frame<xtrd::success>(ro);

    REQUIRE(::stat(path_, &sb) == 0);
}

TEST_CASE_METHOD(fixture, "logger socket path too long test", "[logger]")
{
    constexpr std::size_t path_size =
        sizeof(static_cast<sockaddr_un*>(nullptr)->sun_path);

    std::string path(path_size, 'X');

    {
        capture_cerr capture;

        log_.set_command_path(path);

        const std::string expected =
            "Error: Command path '" + path + "' is too long\n";

        REQUIRE(capture.buf_.str() == expected);
    }
}

TEST_CASE_METHOD(fixture, "logger socket bind error test", "[logger]")
{
    std::string path = "/hopefully/this/path/does/not/exist/6aJ^JwK2+E7fgL*5q";

    {
        capture_cerr capture;

        log_.set_command_path(path);

        const std::string expected =
            "Error: Failed to bind command socket to path '" + path +
            "': No such file or directory\n";

        REQUIRE(capture.buf_.str() == expected);
    }
}

TEST_CASE("logger open path test", "[logger]")
{
    xtr::logger log("/dev/null");

    auto p = log.get_sink("Name");

    XTR_LOG(p, "Test");
}

TEST_CASE_METHOD(fixture, "logger noexcept test", "[logger]")
{
    static_assert(noexcept(XTR_LOG(s_, "Hello world")));
    static_assert(noexcept(XTR_LOG(s_, "Hello world {}", 123)));
    static_assert(noexcept(XTR_LOG(s_, "Hello {}", "world")));

    std::string s1("world");
    CHECK(noexcept(XTR_LOG(s_, "Hello {}", s1)));
    CHECK(noexcept(XTR_LOG(s_, "Hello {}", std::move(s1))));

    std::string_view s2("world");
    CHECK(noexcept(XTR_LOG(s_, "Hello {}", s2)));
    CHECK(noexcept(XTR_LOG(s_, "Hello {}", std::move(s2))));

    const char* s3 = "world";
    CHECK(noexcept(XTR_LOG(s_, "Hello {}", s3)));
    CHECK(noexcept(XTR_LOG(s_, "Hello {}", std::move(s3))));

    const char s4[] = "world";
    CHECK(noexcept(XTR_LOG(s_, "Hello {}", s4)));
    CHECK(noexcept(XTR_LOG(s_, "Hello {}", std::move(s4))));

    move_thrower mt;
    CHECK(noexcept(XTR_LOG(s_, "Hello {}", mt)));
    CHECK(!noexcept(XTR_LOG(s_, "Hello {}", std::move(mt))));

    copy_thrower ct;
    CHECK(!noexcept(XTR_LOG(s_, "Hello {}", ct)));
    CHECK(noexcept(XTR_LOG(s_, "Hello {}", std::move(ct))));
}

TEST_CASE_METHOD(fixture, "logger re-register sink test", "[logger]")
{
    s_.close();
    log_.register_sink(s_, "Reregistered");
    XTR_LOG(s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "I 2000-01-01 01:02:03.123456 Reregistered logger.cpp:{}: Test"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger macro test", "[logger]")
{
    xtr::timespec ts{};

    // This test just exists to check if clang emits warnings for ``must
    // specify at least one argument'' in a variadic macro.

    // XTR_LOG and variants
    XTR_LOG(s_, "Test");
    if (true == false)
        XTR_LOGL(fatal, s_, "Test");
    XTR_LOGL(error, s_, "Test");
    XTR_LOGL(warning, s_, "Test");
    XTR_LOGL(info, s_, "Test");
    XTR_LOGL(debug, s_, "Test");

    // XTR_TRY_LOG and variants
    XTR_TRY_LOG(s_, "Test");
    if (true == false)
        XTR_TRY_LOGL(fatal, s_, "Test");
    XTR_TRY_LOGL(error, s_, "Test");
    XTR_TRY_LOGL(warning, s_, "Test");
    XTR_TRY_LOGL(info, s_, "Test");
    XTR_TRY_LOGL(debug, s_, "Test");

    // XTR_LOG_TS and variants
    XTR_LOG_TS(s_, ts, "Test");
    if (true == false)
        XTR_LOGL_TS(fatal, s_, ts, "Test");
    XTR_LOGL_TS(error, s_, ts, "Test");
    XTR_LOGL_TS(warning, s_, ts, "Test");
    XTR_LOGL_TS(info, s_, ts, "Test");
    XTR_LOGL_TS(debug, s_, ts, "Test");

    // XTR_TRY_LOG_TS and variants
    XTR_TRY_LOG_TS(s_, ts, "Test");
    if (true == false)
        XTR_TRY_LOGL_TS(fatal, s_, ts, "Test");
    XTR_TRY_LOGL_TS(error, s_, ts, "Test");
    XTR_TRY_LOGL_TS(warning, s_, ts, "Test");
    XTR_TRY_LOGL_TS(info, s_, ts, "Test");
    XTR_TRY_LOGL_TS(debug, s_, ts, "Test");

    // XTR_LOG_RTC and variants
    XTR_LOG_RTC(s_, "Test");
    if (true == false)
        XTR_LOGL_RTC(fatal, s_, "Test");
    XTR_LOGL_RTC(error, s_, "Test");
    XTR_LOGL_RTC(warning, s_, "Test");
    XTR_LOGL_RTC(info, s_, "Test");
    XTR_LOGL_RTC(debug, s_, "Test");

    // XTR_TRY_LOG_RTC and variants
    XTR_TRY_LOG_RTC(s_, "Test");
    if (true == false)
        XTR_TRY_LOGL_RTC(fatal, s_, "Test");
    XTR_TRY_LOGL_RTC(error, s_, "Test");
    XTR_TRY_LOGL_RTC(warning, s_, "Test");
    XTR_TRY_LOGL_RTC(info, s_, "Test");
    XTR_TRY_LOGL_RTC(debug, s_, "Test");

    // XTR_LOG_TSC and variants
    XTR_LOG_TSC(s_, "Test");
    if (true == false)
        XTR_LOGL_TSC(fatal, s_, "Test");
    XTR_LOGL_TSC(error, s_, "Test");
    XTR_LOGL_TSC(warning, s_, "Test");
    XTR_LOGL_TSC(info, s_, "Test");
    XTR_LOGL_TSC(debug, s_, "Test");

    // XTR_TRY_LOG_TSC and variants
    XTR_TRY_LOG_TSC(s_, "Test");
    if (true == false)
        XTR_TRY_LOGL_TSC(fatal, s_, "Test");
    XTR_TRY_LOGL_TSC(error, s_, "Test");
    XTR_TRY_LOGL_TSC(warning, s_, "Test");
    XTR_TRY_LOGL_TSC(info, s_, "Test");
    XTR_TRY_LOGL_TSC(debug, s_, "Test");
}

TEST_CASE("default_command_path fallback test", "[logger]")
{
    std::string rundir;
    if (const char* tmp = ::getenv("XDG_RUNTIME_DIR"))
        rundir = tmp;

    std::string tmpdir;
    if (const char* tmp = ::getenv("TMPDIR"))
        tmpdir = tmp;

    CHECK(::setenv("XDG_RUNTIME_DIR", "/no/such/directory", 1) == 0);
    CHECK(::setenv("TMPDIR", "/foo", 1) == 0);
    CHECK(xtr::default_command_path().starts_with("/foo"));

    ::unsetenv("TMPDIR");
    CHECK(xtr::default_command_path().starts_with("/tmp"));

    ::unsetenv("XDG_RUNTIME_DIR");
    CHECK((
        xtr::default_command_path().starts_with("/tmp") ||
        xtr::default_command_path().starts_with("/run/user")));

    if (!rundir.empty())
        ::setenv("XDG_RUNTIME_DIR", rundir.c_str(), 1);
    if (!tmpdir.empty())
        ::setenv("TMPDIR", tmpdir.c_str(), 1);
}

TEST_CASE_METHOD(fixture, "logger custom log level style test", "[logger]")
{
    auto style =
        [](xtr::log_level_t level)
        {
            switch (level)
            {
            case xtr::log_level_t::error:
                return "ERROR ";
            case xtr::log_level_t::warning:
                return "WARNING ";
            case xtr::log_level_t::info:
                return "INFO ";
            case xtr::log_level_t::debug:
                return "DEBUG ";
            default:
                return "";
            }
        };

    log_.set_log_level_style(style);
    s_.set_level(xtr::log_level_t::debug);

    XTR_LOGL(error, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "ERROR 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));

    XTR_LOGL(warning, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "WARNING 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));

    XTR_LOGL(info, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "INFO 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));

    XTR_LOGL(debug, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "DEBUG 2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
}

TEST_CASE_METHOD(fixture, "logger systemd log level style test", "[logger]")
{
    log_.set_log_level_style(xtr::systemd_log_level_style);
    s_.set_level(xtr::log_level_t::debug);

    XTR_LOGL(error, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "<3>2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));

    XTR_LOGL(warning, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "<4>2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));

    XTR_LOGL(info, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "<6>2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));

    XTR_LOGL(debug, s_, "Test"), line_ = __LINE__;
    REQUIRE(last_line() == "<7>2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));

#ifndef XTR_THREAD_SANITIZER_ENABLED
    struct sigaction act{};
    static struct sigaction oldact{};
    static sigjmp_buf jbuf;

    const int val = sigsetjmp(jbuf, 1);

    if (val == 0)
    {
        act.sa_handler =
            [](int)
            {
                siglongjmp(jbuf, 1);
            };

        REQUIRE(::sigaction(SIGABRT, &act, &oldact) == 0);
        line_ = __LINE__, XTR_LOGL(fatal, s_, "Test");
    }

    if (::sigaction(SIGABRT, &oldact, nullptr) != 0)
        err(EXIT_FAILURE, "sigaction failed");
    REQUIRE(last_line() == "<0>2000-01-01 01:02:03.123456 Name logger.cpp:{}: Test"_format(line_));
#endif
}

#if __cpp_exceptions
TEST_CASE("logger open invalid path test", "[logger]")
{
    REQUIRE_THROWS_WITH(
        xtr::logger("/no/such/file"),
        "Failed to open `/no/such/file': No such file or directory");
}
#endif

#ifndef XTR_THREAD_SANITIZER_ENABLED
TEST_CASE_METHOD(fixture, "logger fatal test", "[logger]")
{
    struct sigaction act{};
    static struct sigaction oldact{};
    static sig_atomic_t abort_handler_count = 0;
    static sig_atomic_t abort_handler_signo = 0;
    static sigjmp_buf jbuf;

    const int val = sigsetjmp(jbuf, 1);

    if (val == 0)
    {
        act.sa_handler =
            [](int signo)
            {
                ++abort_handler_count;
                abort_handler_signo = signo;
                siglongjmp(jbuf, 1);
            };

        REQUIRE(::sigaction(SIGABRT, &act, &oldact) == 0);
        XTR_LOGL(fatal, s_, "Fatal error");
    }

    if (::sigaction(SIGABRT, &oldact, nullptr) != 0)
        err(EXIT_FAILURE, "sigaction failed");
    REQUIRE(val == 1);
    REQUIRE(abort_handler_count == 1);
    REQUIRE(abort_handler_signo == SIGABRT);
}
#endif
