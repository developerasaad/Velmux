package com.velmux.mobile.engine

import kotlinx.serialization.KSerializer
import kotlinx.serialization.SerializationException
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.serializer

/**
 * Small helpers over kotlinx.serialization — the ONE Json configuration the
 * engine layer uses. `ignoreUnknownKeys` keeps the client forward-compatible
 * with additive engine fields; `encodeDefaults` keeps request encoding
 * byte-stable for the JSON codec fixtures.
 */
object VelmuxJson {

    val json: Json = Json {
        ignoreUnknownKeys = true
        encodeDefaults = true
        prettyPrint = false
    }

    /** Decodes an envelope document, throwing [EngineException] on {"error":...}. */
    fun parseObject(raw: String): JsonObject {
        val root = parseElement(raw)
        val obj = root as? JsonObject
            ?: throw EngineException(
                EngineErrorCode.INVALID_ARGUMENT,
                "engine returned a non-object payload",
            )
        // The error envelope is {"error":{...}} — a REAL error object.
        // Legit payload fields named "error" (job snapshots carry
        // "error":null / "error":"message" as DATA) must not trip this.
        val errorField = obj["error"]
        if (errorField is JsonObject) {
            throw decodeError(errorField)
        }
        return obj
    }

    /** Parses any payload element (object, array, primitive) from raw JSON. */
    fun parseElement(raw: String): JsonElement =
        json.parseToJsonElement(raw)

    private fun decodeError(element: JsonElement): EngineException {
        return try {
            val error = json.decodeFromJsonElement(EngineError.serializer(), element)
            EngineException(error.code, error.message)
        } catch (malformed: SerializationException) {
            EngineException(
                EngineErrorCode.INVALID_ARGUMENT,
                "malformed error envelope: ${malformed.message}",
            )
        }
    }

    /** Decodes a payload field of the envelope as a typed object. */
    inline fun <reified T> decodePayload(raw: String, serializer: KSerializer<T>): T {
        val obj = parseObject(raw)
        return json.decodeFromJsonElement(serializer, obj)
    }

    /** Decodes an array payload (jobs list, event batch, backends). */
    fun <T> decodeArray(raw: String, element: KSerializer<T>): List<T> {
        val root = parseElement(raw)
        val array = root as? JsonArray
            ?: throw EngineException(
                EngineErrorCode.INVALID_ARGUMENT,
                "engine returned a non-array payload",
            )
        return json.decodeFromJsonElement(ListSerializer(element), array)
    }
}

inline fun <reified T> decodeEnginePayload(raw: String): T =
    VelmuxJson.decodePayload(raw, serializer())

fun decodeEngineError(raw: String): EngineException = try {
    val error = VelmuxJson.json.decodeFromString(EngineError.serializer(), raw)
    EngineException(error.code, error.message)
} catch (malformed: SerializationException) {
    EngineException(EngineErrorCode.INVALID_ARGUMENT, "malformed payload: ${malformed.message}")
}

inline fun <reified T> decodeEngineArray(raw: String): List<T> =
    VelmuxJson.decodeArray(raw, serializer())
