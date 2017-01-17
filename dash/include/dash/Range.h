#ifndef DASH__RANGES_H__INCLUDED
#define DASH__RANGES_H__INCLUDED

/**
 * \defgroup  DashRangeConcept  Multidimensional Range Concept
 *
 * \ingroup DashNDimConcepts
 * \{
 * \par Description
 *
 * Definitions for multidimensional range expressions.
 *
 * \see DashDimensionalConcept
 * \see DashViewConcept
 * \see DashIteratorConcept
 * \see \c dash::view_traits
 *
 * Variables used in the following:
 *
 * - \c r instance of a range model type
 * - \c o index type, representing element offsets in the range and their
 *        distance
 * - \c i iterator referencing elements in the range
 *
 * \par Types
 *
 * \par Expressions
 *
 * Expression               | Returns | Effect | Precondition | Postcondition
 * ------------------------ | ------- | ------ | ------------ | -------------
 * <tt>*dash::begin(r)</tt> |         |        |              | 
 * <tt>r[o]</tt>            |         |        |              | 
 *
 * \par Functions
 *
 * - \c dash::begin
 * - \c dash::end
 * - \c dash::distance
 * - \c dash::size
 *
 * \par Metafunctions
 *
 * - \c dash::is_range<X>
 *
 * \}
 */


#include <dash/Dimensional.h>
#include <type_traits>

namespace dash {

/**
 * \concept{DashRangeConcept}
 */
template <class RangeType>
constexpr auto begin(const RangeType & range) -> decltype(range.begin()) {
  return range.begin();
}

/**
 * \concept{DashRangeConcept}
 */
template <class RangeType>
constexpr auto end(const RangeType & range) -> decltype(range.end()) {
  return range.end();
}

/**
 * \concept{DashRangeConcept}
 */
template <class RangeType>
constexpr auto
size(const RangeType & r) -> decltype(r.size()) {
  return r.size();
}


namespace detail {

template<typename T>
struct _is_range_type
{
private:
  typedef char yes;
  typedef long no;

#ifdef __TODO__
  // Test if dash::begin(x) is valid expression:

  template <typename C> static yes has_dash_begin(
                                     decltype(
                                       dash::begin(
                                         std::move(std::declval<T>())
                                       )
                                     ) * );
  template <typename C> static no  has_dash_begin(...);    

  template <typename C> static yes has_dash_end(
                                     decltype(
                                       dash::end(
                                         std::move(std::declval<T>())
                                       )
                                     ) * );
  template <typename C> static no  has_dash_end(...);    

public:
  enum { value = (
              sizeof(has_dash_begin(static_cast<T*>(nullptr))) == sizeof(yes)
           && sizeof(has_dash_end(static_cast<T*>(nullptr)))   == sizeof(yes)
         ) };

//template<typename C, typename begin_decl =
//                                   decltype(
//                                     dash::begin(
//                                       std::move(std::declval<T>())
//                                     )) >
//static yes has_dash_begin(C *);
#else
  // Test if x.begin() is valid expression and type x::iterator is
  // defined:

  template<typename C, typename C::iterator (C::*)() = &C::begin >
  static yes has_begin(C *);
  static no  has_begin(...);

  template<typename C, typename C::iterator (C::*)() = &C::end >
  static yes has_end(C *);
  static no  has_end(...);

public:
  enum { value = (
              sizeof(has_begin(static_cast<T*>(nullptr))) == sizeof(yes)
           && sizeof(has_end(static_cast<T*>(nullptr)))   == sizeof(yes)
         ) };
#endif
};

} // namespace detail

/**
 * Type trait for testing if `dash::begin<T>` and `dash::end<T>`
 * are defined.
 *
 * In the current implementation, range types must specify the
 * return type of `dash::begin<T>` and `dash::end<T>` as type
 * definition `iterator`.
 *
 * This requirement will become obsolete in the near future.
 *
 *
 * Example:
 *
 * \code
 *   bool g_array_is_range = dash::is_range<
 *                                    dash::Array<int>
 *                                 >::value;
 *   // -> true
 *
 *   bool l_array_is_range = dash::is_range<
 *                                    typename dash::Array<int>::local_type
 *                                 >::value;
 *   // -> true
 *
 *   struct inf_range { 
 *     typedef int           * iterator;
 *     typedef std::nullptr_t  sentinel;
 *
 *     iterator begin() { ... }
 *     sentinel end()   { ... }
 *   };
 *
 *   bool inf_range_is_range = dash::is_range<inf_range>::value;
 *   // -> false
 *   //    because of missing definition
 *   //      iterator dash::end<inf_range> -> iterator
 *
 *   Currently requires specialization as workaround:
 *   template <>
 *   struct is_range<inf_range> : std::integral_value<bool, true> { };
 * \endcode
 */
template <class RangeType>
struct is_range : dash::detail::_is_range_type<RangeType> { };

template <
  class RangeType,
  class Iterator,
  class Sentinel = Iterator>
class RangeBase {
public:
  typedef Iterator iterator;
  typedef Sentinel sentinel;
  typedef typename Iterator::index_type index_type;
protected:
  RangeType & derived() {
    return static_cast<RangeType &>(*this);
  }
  const RangeType & derived() const {
    return static_cast<const RangeType &>(*this);
  }
};

/**
 * Adapter template for range concept, wraps `begin` and `end` iterators
 * in range type.
 */
template <
  class Iterator,
  class Sentinel = Iterator>
class IteratorRange
: public RangeBase< IteratorRange<Iterator, Sentinel>,
                    Iterator,
                    Sentinel >
{
  Iterator _begin;
  Sentinel _end;

public:
  template <class Container>
  constexpr explicit IteratorRange(Container && c)
  : _begin(c.begin())
  , _end(c.end())
  { }

  constexpr IteratorRange(Iterator begin, Sentinel end)
  : _begin(std::move(begin))
  , _end(std::move(end))
  { }

  constexpr Iterator begin() const { return _begin; }
  constexpr Iterator end()   const { return _end;   }
};

/**
 * Adapter utility function.
 * Wraps `begin` and `end` iterators in range type.
 */
template <class Iterator, class Sentinel>
constexpr dash::IteratorRange<Iterator, Sentinel>
make_range(Iterator begin, Sentinel end) {
  return dash::IteratorRange<Iterator, Sentinel>(
           std::move(begin),
           std::move(end));
}

} // namespace dash

#include <dash/algorithm/LocalRange.h>
#include <dash/algorithm/LocalRanges.h>

#endif // DASH__RANGES_H__INCLUDED