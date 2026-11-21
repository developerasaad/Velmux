package com.velmux.mobile

import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.ReadableArray
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.bridge.ReadableType
import com.facebook.react.bridge.WritableArray
import com.facebook.react.bridge.WritableMap
import org.json.JSONArray
import org.json.JSONObject

/**
 * Conversions between engine JSON (the bridge contract) and React Native
 * bridge values. Only the shapes the TurboModule contract uses appear here.
 */
internal object JsonBridge {

  // -- engine JSON -> RN values ------------------------------------------------

  fun toWritableMap(json: JSONObject): WritableMap {
    val map = Arguments.createMap()
    for (key in json.keys()) {
      when {
        json.isNull(key) -> map.putNull(key)
        else ->
          when (val value = json.get(key)) {
            is Boolean -> map.putBoolean(key, value)
            is Int -> {
              // JS numbers are doubles; RN reads them back as numbers.
              map.putDouble(key, value.toDouble())
            }
            is Long -> map.putDouble(key, value.toDouble())
            is Double ->
              if (value.isNaN() || value.isInfinite()) {
                map.putNull(key)
              } else {
                map.putDouble(key, value)
              }
            is String -> map.putString(key, value)
            is JSONObject -> map.putMap(key, toWritableMap(value))
            is JSONArray -> map.putArray(key, toWritableArray(value))
            else -> map.putString(key, value.toString())
          }
      }
    }
    return map
  }

  fun toWritableArray(json: JSONArray): WritableArray {
    val array = Arguments.createArray()
    for (i in 0 until json.length()) {
      when {
        json.isNull(i) -> array.pushNull()
        else ->
          when (val value = json.get(i)) {
            is Boolean -> array.pushBoolean(value)
            is Int -> array.pushDouble(value.toDouble())
            is Long -> array.pushDouble(value.toDouble())
            is Double ->
              if (value.isNaN() || value.isInfinite()) {
                array.pushNull()
              } else {
                array.pushDouble(value)
              }
            is String -> array.pushString(value)
            is JSONObject -> array.pushMap(toWritableMap(value))
            is JSONArray -> array.pushArray(toWritableArray(value))
            else -> array.pushString(value.toString())
          }
      }
    }
    return array
  }

  // -- RN values -> engine JSON ---------------------------------------------------

  fun toJsonString(map: ReadableMap): String = toJson(map).toString()

  private fun toJson(map: ReadableMap): JSONObject {
    val json = JSONObject()
    val iterator = map.keySetIterator()
    while (iterator.hasNextKey()) {
      val key = iterator.nextKey()
      when (map.getType(key)) {
        ReadableType.Null -> json.put(key, JSONObject.NULL)
        ReadableType.Boolean -> json.put(key, map.getBoolean(key))
        ReadableType.Number -> json.put(key, map.getDouble(key))
        ReadableType.String -> json.put(key, map.getString(key))
        ReadableType.Map -> map.getMap(key)?.let { json.put(key, toJson(it)) }
        ReadableType.Array -> map.getArray(key)?.let { json.put(key, toJson(it)) }
      }
    }
    return json
  }

  private fun toJson(array: ReadableArray): JSONArray {
    val json = JSONArray()
    for (i in 0 until array.size()) {
      when (array.getType(i)) {
        ReadableType.Null -> json.put(JSONObject.NULL)
        ReadableType.Boolean -> json.put(array.getBoolean(i))
        ReadableType.Number -> json.put(array.getDouble(i))
        ReadableType.String -> json.put(array.getString(i))
        ReadableType.Map -> array.getMap(i)?.let { json.put(toJson(it)) }
        ReadableType.Array -> array.getArray(i)?.let { json.put(toJson(it)) }
      }
    }
    return json
  }
}
