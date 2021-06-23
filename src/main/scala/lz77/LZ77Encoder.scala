package lz77.util

import chisel3._
import chisel3.util._
import lz77Parameters._

class LZ77Encoder(params: lz77Parameters) extends Module {
  val io = IO(new Bundle {
    val out = DecoupledStream(params.compressorMaxCharactersOut, UInt(params.characterBits.W))
    val matchLength = Input(UInt(params.camCharacterSequenceLengthBits.W))
    val matchCAMAddress = Input(UInt(params.camAddressBits.W))
  })
  
  val remainingLengthReg = RegInit(0.U(params.camCharacterSequenceLengthBits.W))
  val minEncodingReg = Reg(Vec(params.minEncodingWidth / params.characterBits, UInt(params.characterBits.W)))
  val minEncodingIndexReg = RegInit(log2Ceil(params.minEncodingWidth / params.characterBits + 1).U)
  
  val remainingLength = WireDefault(remainingLengthReg)
  val minEncoding = WireDefault(minEncodingReg)
  val minEncodingIndex = WireDefault(minEncodingIndexReg)
  
  remainingLengthReg := remainingLength
  minEncodingReg := minEncoding
  minEncodingIndexReg := minEncodingIndex
  
  io.out.finished := remainingLengthReg === 0.U
  
  when(io.matchLength =/= 0.U) {
    remainingLength := io.matchLength + (params.minEncodingWidth / params.characterBits * params.extraCharacterLengthIncrease - params.maxCharactersInMinEncoding).U
    
    val minEncodingUInt = params.escapeCharacter.U ## params.escapeCharacter.U.apply(params.characterBits - 1) ## io.matchCAMAddress ## ((io.matchLength - params.minCharactersToEncode.U) min (1 << params.minEncodingSequenceLengthBits - 1).U)(params.minEncodingSequenceLengthBits - 1, 0)
    minEncoding := (0 until (params.minEncodingWidth / params.characterBits) reverse).map{i => minEncodingUInt((i + 1) * params.characterBits - 1, i * params.characterBits)}
    minEncodingIndex := 0.U
  }
  
  io.out.valid := 0.U
  io.out.bits := DontCare
  for(index <- 0 until params.compressorMaxCharactersOut) {
    val output = io.out.bits(index)
    when(minEncodingIndex < (params.minEncodingWidth / params.characterBits - index).U) {
      output := minEncoding(minEncodingIndex + index.U)
      io.out.valid := (index + 1).U
      when(index.U < io.out.ready) {
        remainingLengthReg := remainingLength - (index * params.extraCharacterLengthIncrease).U
        minEncodingIndexReg := minEncodingIndex + (index + 1).U
      }
    }.elsewhen(remainingLength > ((index + 1) * params.extraCharacterLengthIncrease).U) {
      output := params.maxCharacterValue.U
      io.out.valid := (index + 1).U
      when(index.U < io.out.ready) {
        remainingLengthReg := remainingLength - (index * params.extraCharacterLengthIncrease).U
        minEncodingIndexReg := (params.minEncodingWidth / params.characterBits).U
      }
    }.elsewhen(remainingLength > (index * params.extraCharacterLengthIncrease).U) {
      output := remainingLength - (index * params.extraCharacterLengthIncrease + 1).U
      io.out.valid := (index + 1).U
      when(index.U < io.out.ready) {
        remainingLengthReg := 0.U
        minEncodingIndexReg := (params.minEncodingWidth / params.characterBits).U
      }
    } otherwise {
      // output := DontCare
      // when(index.U < io.out.ready) {
      //   remainingLengthReg := 0
      //   minEncodingIndexReg := (minEncodingWidth / characterBits).U
      // }
    }
  }
}