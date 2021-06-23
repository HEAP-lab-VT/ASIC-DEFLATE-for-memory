package lz77

import lz77Decompressor._
import lz77Parameters._
import chisel3._
import chisel3.util._
import chisel3.iotesters._
import org.scalatest._
import org.scalatest.flatspec._
import matchers.should._
import chisel3.tester._
import chisel3.experimental.BundleLiterals._
import chisel3.stage.ChiselStage
import java.io._
import scala.util.control.Breaks._

class LZ77DecompressorTester(
    lz77: lz77Decompressor,
    params: lz77Parameters,
    input: Seq[Int],
    expected: Seq[Int])
    extends PeekPokeTester[lz77Decompressor](lz77) {
  
  // initialize inputs
  for(index <- 0 until lz77.io.in.bits.length)
    poke(lz77.io.in.bits(index), 0)
  poke(lz77.io.in.valid, 0)
  poke(lz77.io.out.ready, 0)
  poke(lz77.io.in.finished, false)
  
  var inidx = 0
  var outidx = 0
  
  var timeout = 10000
  while(peek(lz77.io.out.finished) == 0 && timeout > 0) {
    poke(lz77.io.in.valid, 0)
    poke(lz77.io.out.ready, 0)
    poke(lz77.io.in.finished, true)
    for(i <- 0 until
        (lz77.io.in.bits.length min (input.length - inidx))) {
      poke(lz77.io.in.bits(i), input(inidx + i))
      poke(lz77.io.in.valid, i + 1)
      poke(lz77.io.in.finished, false)
    }
    
    poke(lz77.io.out.ready, lz77.io.out.bits.length)
    
    // println(s"out valid = ${peek(lz77.io.out.valid)}; in ready = ${peek(lz77.io.in.ready)}")
    
    inidx += (peek(lz77.io.in.ready) min (peek(lz77.io.in.valid))).intValue
    
    for(i <- 0 until
        (peek(lz77.io.out.ready) min (peek(lz77.io.out.valid))).intValue) {
      if(outidx + i < expected.length)
        expect(lz77.io.out.bits(i), expected(outidx + i))
      else {
        println(s"${i}: ${peek(lz77.io.out.bits(i))} (indexed past end of data: ${outidx + i})")
        fail
      }
    }
    
    outidx += (peek(lz77.io.out.ready) min (peek(lz77.io.out.valid))).intValue
    
    step(1)
    timeout -= 1
  }
  
  if(outidx != expected.length) {
    println(s"outidx was ${outidx}; Expected ${expected.length}")
    fail
  }
}


class LZ77DecompressorTest extends AnyFlatSpec with Matchers {
  
  def generateCompressed(
      params: lz77Parameters,
      len: Int = 4096,
      overlap: Boolean = true
  ): Tuple2[Seq[Int], Seq[Int]] = {
    var expect = Seq.fill(0){(scala.math.random() * 256).toInt}
    var input = expect.flatMap(d =>
      if(d == params.escapeCharacter) Seq(d, d) else Seq(d))
    while(expect.length < len) breakable {
      if(scala.math.random() >= .1 ||
          expect.length + params.minCharactersToEncode >= len ||
          expect.length == 0) {
        val char = (scala.math.random() * 256).toInt
        expect :+= char
        input :+= char
        if(char == params.escapeCharacter)
          input :+= char
      }
      else {
        val index = params.camCharacters - 1 - (scala.math.random() *
          (expect.length min params.camCharacters)).toInt
        var length = params.minCharactersToEncode
        val p = scala.math.random() * .3 + .7
        while(scala.math.random() < p && length < len - expect.length)
          length += 1
        if(index + length > params.camCharacters && !overlap) break // continue
        
        var encoding : BigInt = 0
        encoding <<= params.characterBits
        encoding |= params.escapeCharacter
        encoding <<= 1
        encoding |= ~params.escapeCharacter >> (params.characterBits - 1) & 1
        encoding <<= params.camAddressBits
        encoding |= index
        
        encoding <<= params.minEncodingSequenceLengthBits
        if(length <= params.maxCharactersInMinEncoding) {
          encoding |= length - params.minCharactersToEncode
        }
        else {
          encoding |= (1 << params.minEncodingSequenceLengthBits) - 1
          var remaining = length - params.maxCharactersInMinEncoding - 1
          while(remaining >= 0) {
            encoding <<= params.characterBits
            encoding |= params.maxCharacterValue min remaining
            remaining -= params.extraCharacterLengthIncrease
          }
        }
        
        val encodinglength =
          if(length <= params.maxCharactersInMinEncoding)
            params.minEncodingWidth / params.characterBits
          else if(length == params.maxPatternLength)
            params.maxEncodingCharacterWidths
          else
            (params.minEncodingWidth / params.characterBits) + 1 +
              ((length - params.maxCharactersInMinEncoding) /
                params.maxCharacterValue)
        
        for(i <- 0 until encodinglength reverse) {
          input :+= (encoding >> (i * params.characterBits) &
            params.maxCharacterValue).toInt
        }
        expect ++= Iterator.continually(
          (Seq.fill(params.camCharacters)(0) ++ expect)
          .takeRight(params.camCharacters)
          .drop(index))
          .flatten
          .take(length)
      }
    }
    
    (input, expect)
  }
  
  "LZ77 decompressor" should "passthrough uncompressed data" in {
    val params = new getLZ77FromCSV().getLZ77FromCSV("configFiles/lz77.csv")
    val expect = Seq.fill(1000){(scala.math.random() * 256).toInt}
    val input =
      expect.flatMap(d => if(d == params.escapeCharacter) Seq(d, d) else Seq(d))
    
    chisel3.iotesters.Driver(() => new lz77Decompressor(params)){lz77 =>
      new LZ77DecompressorTester(lz77, params, input, expect)
    } should be (true)
  }
  
  "LZ77 decompressor" should "decompress compressed data (no overlap)" in {
    val params = new getLZ77FromCSV().getLZ77FromCSV("configFiles/lz77.csv")
    var (input, expect) = generateCompressed(params, 10000, false)
    
    chisel3.iotesters.Driver.execute(Array(),
      () => new lz77Decompressor(params))
    {lz77 =>
      new LZ77DecompressorTester(lz77, params, input, expect)
    } should be (true)
  }
  
  "LZ77 decompressor" should "decompress compressed data (yes overlap)" in {
    val params = new getLZ77FromCSV().getLZ77FromCSV("configFiles/lz77.csv")
    var (input, expect) = generateCompressed(params, 10000, true)
    
    chisel3.iotesters.Driver.execute(Array(),
      () => new lz77Decompressor(params))
    {lz77 =>
      new LZ77DecompressorTester(lz77, params, input, expect)
    } should be (true)
  }
}