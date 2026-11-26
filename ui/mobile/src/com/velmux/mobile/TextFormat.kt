package com.velmux.mobile

import kotlin.math.abs
import kotlin.math.pow
import kotlin.math.roundToLong

/**
 * The shared display-string formatter: the exact printf subset the UI
 * uses — %s, %d, %02d-style zero-padded integers, %.Nf and %0W.NF
 * floats — implemented in common Kotlin so display strings build
 * identically on every platform (the JVM's String.format is JVM-only;
 * commonMain cannot use it).
 */
private fun formatShared(template: String, vararg args: Any?): String {
    val out = StringBuilder()
    var argIndex = 0
    var i = 0
    while (i < template.length) {
        val c = template[i]
        if (c != '%' || i + 1 >= template.length) {
            out.append(c)
            i++
            continue
        }
        var j = i + 1
        if (template[j] == '%') {
            out.append('%')
            i = j + 1
            continue
        }
        var zeroPad = false
        if (template[j] == '0') {
            zeroPad = true
            j++
        }
        var width = 0
        while (j < template.length && template[j].isDigit()) {
            width = width * 10 + (template[j] - '0')
            j++
        }
        var precision = -1
        if (j < template.length && template[j] == '.') {
            j++
            precision = 0
            while (j < template.length && template[j].isDigit()) {
                precision = precision * 10 + (template[j] - '0')
                j++
            }
        }
        if (j >= template.length) {
            out.append(c)
            i++
            continue
        }
        val arg = args.getOrElse(argIndex) { "" }
        argIndex++
        when (val conversion = template[j]) {
            's' -> pad(out, arg.toString(), width, zeroPad)
            'd' -> {
                val number = when (arg) {
                    is Number -> arg.toLong()
                    else -> arg.toString().toLongOrNull() ?: 0L
                }
                pad(out, number.toString(), width, zeroPad)
            }
            'f' -> {
                val number = when (arg) {
                    is Number -> arg.toDouble()
                    else -> arg.toString().toDoubleOrNull() ?: 0.0
                }
                pad(
                    out,
                    fixedDecimals(number, if (precision >= 0) precision else 6),
                    width,
                    zeroPad,
                )
            }
            else -> out.append('%').append(conversion)
        }
        i = j + 1
    }
    return out.toString()
}

/** Pads [text] to [width] (left pad; '0' when [zeroPad], spaces otherwise). */
private fun pad(out: StringBuilder, text: String, width: Int, zeroPad: Boolean) {
    if (width <= text.length) {
        out.append(text)
        return
    }
    val filler = if (zeroPad) '0' else ' '
    repeat(width - text.length) { out.append(filler) }
    out.append(text)
}

/** Fixed-decimal rendering with half-away-from-zero rounding (printf). */
private fun fixedDecimals(value: Double, decimals: Int): String {
    if (decimals <= 0) {
        return value.roundToLong().toString()
    }
    val scale = 10.0.pow(decimals)
    val rounded = (value * scale).roundToLong()
    val sign = if (rounded < 0) "-" else ""
    val digits = abs(rounded).toString()
    val whole = if (digits.length <= decimals) "0" else digits.substring(0, digits.length - decimals)
    val fraction = digits.substring((digits.length - decimals).coerceAtLeast(0))
        .padStart(decimals, '0')
    return "$sign$whole.$fraction"
}

/**
 * The common replacement for the JVM-only String.format extension: every
 * display template in commonMain formats through this (identical output
 * on JVM and Kotlin/Native).
 */
fun String.fmt(vararg args: Any?): String = formatShared(this, *args)
