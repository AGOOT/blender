/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "DNA_node_types.h"

#include "BLI_any.hh"
#include "BLI_cpp_type.hh"
#include "BLI_generic_pointer.hh"

namespace blender::bke {

/**
 * #SocketValueVariant is used by geometry nodes in the lazy-function evaluator to pass data
 * between nodes. Specifically, it is the container type for the following socket types: bool,
 * float, integer, vector, rotation, color and string.
 *
 * The data passed through e.g. an integer socket can be a single value, a field or a grid (and in
 * the lists and images). Each of those is stored differently, but this container can store them
 * all.
 *
 * A key requirement for this container is that it is type-erased, i.e. not all code that uses it
 * has to include all the headers required to process the other storage types. This is achieved by
 * using the #Any type and by providing templated accessors that are implemented outside of a
 * header.
 */
class SocketValueVariant {
 private:
  /**
   * This allows faster lookup of the correct type in the #Any below. For example, when retrieving
   * the value of an integer socket, we'd usually have to check whether the #Any contains a single
   * `int` or a field. Doing that check by comparing an enum is cheaper.
   *
   * Also, to figure out if we currently store a single value we'd otherwise have to check whether
   * they #Any stored an integer or float or boolean etc.
   */
  enum class Kind {
    /**
     * Used to indicate that there is no value currently. This is used by the default constructor.
     */
    None,
    /**
     * Indicates that there is a single value like `int`, `float` or `std::string` stored.
     */
    Single,
    /**
     * Indicates that there is a `GField` stored.
     */
    Field,
    /**
     * Indicates that there is a `GVolumeGrid` stored.
     */
    Grid,
  };

  /**
   * High level category of the stored type.
   */
  Kind kind_ = Kind::None;
  /**
   * The socket type that corresponds to the stored value type, e.g. `SOCK_INT` for an `int` or
   * integer field.
   */
  eNodeSocketDatatype socket_type_;
  /**
   * Contains the actual socket value. For single values this contains the value directly (e.g.
   * `int` or `float3`). For fields this always contains a #GField and not e.g. #Field<int>. This
   * simplifies generic code.
   *
   * Small types are embedded directly, while larger types are separately allocated.
   */
  Any<void, 24> value_;

 public:
  /**
   * Create an empty variant. This is not valid for any socket type yet.
   */
  SocketValueVariant() = default;

  /**
   * Create a variant based on the given value. This works for primitive types, #GField and
   * #Field<T>.
   */
  template<typename T> explicit SocketValueVariant(T &&value);

  /**
   * \return True if the stored value is valid for a specific socket type. This is mainly meant to
   * be used by asserts.
   */
  bool valid_for_socket(eNodeSocketDatatype socket_type) const;

  /**
   * Get the stored value as a specific type. For convenience this allows accessing the stored type
   * as a different type. For example, a stored single `int` can also be accessed as `GField` or
   * `Field<int>` (but not `float` or `Field<float>`).
   *
   * This method may leave the variant empty, in a moved from state or unchanged. Therefore, this
   * should only be called once.
   */
  template<typename T> T extract();

  /**
   * Same as #extract, but always leaves the variant unchanged. So this method can be called
   * multiple times.
   */
  template<typename T> T get() const;

  /**
   * Replaces the stored value with a new value of potentially a different type.
   */
  template<typename T> void set(T &&value);

  /**
   * If true, the stored value cannot be converted to a single value without loss of information.
   */
  bool is_context_dependent_field() const;

  /**
   * Convert the stored value into a single value. For simple value access, this is not necessary,
   * because #get` does the conversion implicitly. However, it is necessary if one wants to use
   * #get_single_ptr. Context-dependend fields or grids will just result in a fallback value.
   *
   * The caller has to make sure that the stored value is a single value, field or grid.
   */
  void convert_to_single();

  /**
   * Get a pointer to the embedded single value. The caller has to make sure that there actually is
   * a single value stored, e.g. by calling #convert_to_single.
   */
  GPointer get_single_ptr() const;
  GMutablePointer get_single_ptr();

  /**
   * Replace the stored value with the given single value.
   */
  void store_single(eNodeSocketDatatype socket_type, const void *value);

  /**
   * Replaces the stored value with a new value-initialized single value of the given type and
   * returns a pointer to the value. The caller can then write a different value of the same type
   * into its place.
   */
  void *new_single_for_write(eNodeSocketDatatype socket_type);
  void *new_single_for_write(const CPPType &cpp_type);

  friend std::ostream &operator<<(std::ostream &stream, const SocketValueVariant &value_variant);

 private:
  /**
   * This exists so that only one instance of the underlying template has to be instantiated per
   * type. So only `store_impl<int>` is necessary, but not `store_impl<const int &>`.
   */
  template<typename T> void store_impl(T value);
};

template<typename T> inline SocketValueVariant::SocketValueVariant(T &&value)
{
  this->set(std::forward<T>(value));
}

template<typename T> inline void SocketValueVariant::set(T &&value)
{
  this->store_impl<std::decay_t<T>>(std::forward<T>(value));
}

}  // namespace blender::bke
