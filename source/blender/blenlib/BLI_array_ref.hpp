/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup bli
 *
 * An ArrayRef references some memory buffer owned
 * by someone else. If possible, functions should take
 * an ArrayRef as input. This allows passing on different
 * kinds of class types without doing unnecessary conversions.
 *
 * ArrayRef instances should be passed by value.
 */

#pragma once

#include <vector>
#include <array>
#include <algorithm>
#include <iostream>
#include <string>

#include "BLI_utildefines.h"
#include "BLI_memory.hpp"

namespace BLI {

template<typename T> class ArrayRef {
 private:
  const T *m_start = nullptr;
  uint m_size = 0;

 public:
  /**
   * Create a reference to an empty array.
   * The pointer is allowed to be nullptr.
   */
  ArrayRef() = default;

  ArrayRef(const T *start, uint size) : m_start(start), m_size(size)
  {
  }

  ArrayRef(const std::initializer_list<T> &list) : ArrayRef(list.begin(), list.size())
  {
  }

  ArrayRef(const std::vector<T> &vector) : ArrayRef(vector.data(), vector.size())
  {
  }

  template<std::size_t N> ArrayRef(const std::array<T, N> &array) : ArrayRef(array.data(), N)
  {
  }

  /**
   * Return a continuous part of the array.
   * This will assert when the slice is out of bounds.
   */
  ArrayRef slice(uint start, uint length) const
  {
    BLI_assert(start + length <= this->size());
    return ArrayRef(m_start + start, length);
  }

  /**
   * Return a new ArrayRef with n elements removed from the beginning.
   */
  ArrayRef drop_front(uint n = 1) const
  {
    BLI_assert(n <= this->size());
    return this->slice(n, this->size() - n);
  }

  /**
   * Return a new ArrayRef with n elements removed from the beginning.
   */
  ArrayRef drop_back(uint n = 1) const
  {
    BLI_assert(n <= this->size());
    return this->slice(0, this->size() - n);
  }

  /**
   * Return a new ArrayRef that only contains the first n elements.
   */
  ArrayRef take_front(uint n) const
  {
    BLI_assert(n <= this->size());
    return this->slice(0, n);
  }

  /**
   * Return a new ArrayRef that only contains the last n elements.
   */
  ArrayRef take_back(uint n) const
  {
    BLI_assert(n <= this->size());
    return this->slice(this->size() - n, n);
  }

  /**
   * Copy the values in this array to another array.
   */
  void copy_to(T *ptr) const
  {
    BLI::copy_n(m_start, m_size, ptr);
  }

  const T *begin() const
  {
    return m_start;
  }

  const T *end() const
  {
    return m_start + m_size;
  }

  const T &operator[](uint index) const
  {
    BLI_assert(index < m_size);
    return m_start[index];
  }

  const T *data() const
  {
    return m_start;
  }

  /**
   * Return the number of elements in the referenced array.
   */
  uint size() const
  {
    return m_size;
  }

  /**
   * Return the number of bytes referenced by this ArrayRef.
   */
  uint byte_size() const
  {
    return sizeof(T) * m_size;
  }

  /**
   * Does a linear search to see of the value is in the array.
   * Return true if it is, otherwise false.
   */
  bool contains(const T &value) const
  {
    for (const T &element : *this) {
      if (element == value) {
        return true;
      }
    }
    return false;
  }

  /**
   * Does a constant time check to see if the pointer is within the referenced array.
   * Return true if it is, otherwise false.
   */
  bool contains_ptr(const T *ptr) const
  {
    return (this->begin() <= ptr) && (ptr < this->end());
  }

  /**
   * Does a linear search to count how often the value is in the array.
   * Returns the number of occurences.
   */
  uint count(const T &value) const
  {
    uint counter = 0;
    for (const T &element : *this) {
      if (element == value) {
        counter++;
      }
    }
    return counter;
  }

  /**
   * Return a reference to the first element in the array.
   * Asserts when the array is empty.
   */
  const T &first() const
  {
    BLI_assert(m_size > 0);
    return m_start[0];
  }

  /**
   * Return a reference to the last elemeent in the array.
   * Asserts when the array is empty.
   */
  const T &last() const
  {
    BLI_assert(m_size > 0);
    return m_start[m_size - 1];
  }

  /**
   * Get element at the given index. If the index is out of range, return the fallback value.
   */
  T get(uint index, const T &fallback)
  {
    if (index < m_size) {
      return m_start[index];
    }
    return fallback;
  }

  /**
   * Get a new array ref to the same underlying memory buffer. No conversions are done.
   * Asserts when the sizes of the types don't match.
   */
  template<typename NewT> ArrayRef<NewT> cast() const
  {
    /* Can be adjusted to allow different type sizes when necessary. */
    BLI_STATIC_ASSERT(sizeof(T) == sizeof(NewT), "");
    return ArrayRef<NewT>((NewT *)m_start, m_size);
  }

  /**
   * A debug utility to print the content of the array ref. Every element will be printed on a
   * separate line using the given callback.
   */
  template<typename PrintLineF> void print_as_lines(std::string name, PrintLineF print_line) const
  {
    std::cout << "ArrayRef: " << name << " \tSize:" << m_size << '\n';
    for (const T &value : *this) {
      std::cout << "  ";
      print_line(value);
      std::cout << '\n';
    }
  }
};

template<typename T> class MutableArrayRef {
 private:
  T *m_start;
  uint m_size;

 public:
  MutableArrayRef() = default;

  MutableArrayRef(T *start, uint size) : m_start(start), m_size(size)
  {
  }

  MutableArrayRef(std::initializer_list<T> &list) : MutableArrayRef(list.begin(), list.size())
  {
  }

  MutableArrayRef(std::vector<T> &vector) : MutableArrayRef(vector.data(), vector.size())
  {
  }

  template<std::size_t N>
  MutableArrayRef(std::array<T, N> &array) : MutableArrayRef(array.data(), N)
  {
  }

  operator ArrayRef<T>()
  {
    return ArrayRef<T>(this->data(), this->size());
  }

  T *data() const
  {
    return m_start;
  }

  uint size() const
  {
    return m_size;
  }

  /**
   * Replace all elements in the referenced array with the given value.
   */
  void fill(const T &element)
  {
    std::fill_n(m_start, m_size, element);
  }

  /**
   * Replace a subset of all elements with the given value.
   */
  void fill_indices(ArrayRef<uint> indices, const T &element)
  {
    for (uint i : indices) {
      m_start[i] = element;
    }
  }

  /**
   * Copy the values from another array into the references array.
   */
  void copy_from(const T *ptr)
  {
    BLI::copy_n(ptr, m_size, m_start);
  }

  void copy_from(ArrayRef<T> other)
  {
    BLI_assert(this->size() == other.size());
    this->copy_from(other.begin());
  }

  T *begin() const
  {
    return m_start;
  }

  T *end() const
  {
    return m_start + m_size;
  }

  T &operator[](uint index) const
  {
    BLI_assert(index < this->size());
    return m_start[index];
  }

  /**
   * Return a continuous part of the array.
   * This will assert when the slice is out of bounds.
   */
  MutableArrayRef slice(uint start, uint length) const
  {
    BLI_assert(start + length <= this->size());
    return MutableArrayRef(m_start + start, length);
  }

  /**
   * Return a new MutableArrayRef with n elements removed from the beginning.
   */
  MutableArrayRef drop_front(uint n = 1) const
  {
    BLI_assert(n <= this->size());
    return this->slice(n, this->size() - n);
  }

  /**
   * Return a new MutableArrayRef with n elements removed from the beginning.
   */
  MutableArrayRef drop_back(uint n = 1) const
  {
    BLI_assert(n <= this->size());
    return this->slice(0, this->size() - n);
  }

  /**
   * Return a new MutableArrayRef that only contains the first n elements.
   */
  MutableArrayRef take_front(uint n) const
  {
    BLI_assert(n <= this->size());
    return this->slice(0, n);
  }

  /**
   * Return a new MutableArrayRef that only contains the last n elements.
   */
  MutableArrayRef take_back(uint n) const
  {
    BLI_assert(n <= this->size());
    return this->slice(this->size() - n, n);
  }
};

/**
 * Shorthand to make use of automatic template parameter deduction.
 */
template<typename T> ArrayRef<T> ref_c_array(T *array, uint size)
{
  return ArrayRef<T>(array, size);
}

} /* namespace BLI */
