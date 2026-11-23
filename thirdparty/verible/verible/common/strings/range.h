// Copyright 2017-2020 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef VERIBLE_COMMON_STRINGS_RANGE_H_
#define VERIBLE_COMMON_STRINGS_RANGE_H_

#include <string_view>
#include <utility>

namespace verible {

// Construct a string_view from two end-points.
// string_view lacks the two-iterator constructor that (iterator) ranges and
// containers do.
// Note, this can go with c++20 built-in string_view constructor.
std::string_view make_string_view_range(std::string_view::const_iterator begin,
                                        std::string_view::const_iterator end);

// [fpgalsp patch] 指针重载：调用方常常需要跨**不同的** string_view 对象取区间
//   （例如从一个 token 的结尾到下一个 token 的开头）。用迭代器做这件事在 MSVC 的
//   受检迭代器（Debug）下会断言 "cannot subtract incompatible string_view iterators"；
//   用 (const char*, const char*) 只是普通的指针运算。
std::string_view make_string_view_range(const char *begin, const char *end);

// Returns [x,y] where superstring.substr(x, y-x) == substring.
// Precondition: substring must be a sub-range of superstring.
std::pair<int, int> SubstringOffsets(std::string_view substring,
                                     std::string_view superstring);

}  // namespace verible

#endif  // VERIBLE_COMMON_STRINGS_RANGE_H_
