#ifndef VALHALLA_BALDR_TRANSITSTOP_H_
#define VALHALLA_BALDR_TRANSITSTOP_H_

#include <valhalla/baldr/graphconstants.h>

namespace valhalla {
namespace baldr {

/**
 * Information held for each transit stop. This is information not required
 * during path generation. Such information is held within NodeInfo (lat,lng,
 * type, etc.).
 */
class TransitStop {
 public:
  // Constructor with arguments
  TransitStop(const uint32_t stopid, const uint32_t one_stop_offset,
              const uint32_t name_offset);

  /**
   * Get the internal stop Id.
   * @return  Returns the internal stop Id.
   */
  uint32_t stopid() const;

  /**
   * Get the TransitLand one stop Id offset for the stop.
   * @return  Returns the TransitLand one stop Id offset.
   */
  uint32_t one_stop_offset() const;

  /**
   * Get the text/name offset for the stop name.
   * @return  Returns the name offset in the text/name list.
   */
  uint32_t name_offset() const;

  /**
   * operator < - for sorting. Sort by stop Id.
   * @param  other  Other transit stop to compare to.
   * @return  Returns true if stop Id < other stop Id.
   */
  bool operator < (const TransitStop& other) const;

 protected:
  // Internal stop Id. Used to lookup/index stops.
  uint32_t stopid_;

  // TransitLand one stop Id offset.
  uint32_t one_stop_offset_;

  // Stop name offset in the text/name list.
  uint32_t name_offset_;

};

}
}

#endif  // VALHALLA_BALDR_TRANSITSTOP_H_
