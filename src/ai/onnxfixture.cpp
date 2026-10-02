// 层：功能
#include "onnxfixture.h"

#include <QFile>

#include <cstring>

// ai/ — protobuf 线格式直排器（varint/tag/length-delimited/fixed32）。
// ONNX 字段号（onnx.proto 稳定面）：
//   ModelProto          ir_version=1 producer_name=2 graph=7 opset_import=8
//   OperatorSetIdProto  domain=1 version=2
//   GraphProto          node=1 name=2 initializer=5 input=11 output=12
//   NodeProto           input=1 output=2 name=3 op_type=4
//   ValueInfoProto      name=1 type=2
//   TypeProto           tensor_type=1；Tensor: elem_type=1 shape=2
//   TensorShapeProto    dim=1；Dimension: dim_param=1 dim_value=2
//   TensorProto         dims=1(packed) data_type=2 name=8 raw_data=9
// elem types：FLOAT=1（本生成器只产 float32）。

namespace
{
class Pb
{
  public:
    QByteArray bytes;

    void varint( uint64_t v )
    {
      while ( v >= 0x80 )
      {
        bytes.append( char( uint8_t( v ) | 0x80 ) );
        v >>= 7;
      }
      bytes.append( char( uint8_t( v ) ) );
    }
    void tag( int field, int wire ) { varint( ( uint64_t( field ) << 3 ) | uint64_t( wire ) ); }
    // wire 0=varint 2=length-delimited 5=fixed32
    void varintField( int field, uint64_t v )
    {
      tag( field, 0 );
      varint( v );
    }
    void lenField( int field, const QByteArray &payload )
    {
      tag( field, 2 );
      varint( uint64_t( payload.size() ) );
      bytes += payload;
    }
    void strField( int field, const char *s )
    {
      const int n = int( std::strlen( s ) );
      tag( field, 2 );
      varint( uint64_t( n ) );
      bytes.append( s, n );
    }
    void packedInt64Field( int field, const QVector<int64_t> &vals )
    {
      Pb inner;
      for ( int64_t v : vals )
        inner.varint( uint64_t( v ) );
      lenField( field, inner.bytes );
    }
};

QByteArray rawFloatBytes( const QVector<float> &vals )
{
  QByteArray raw;
  raw.resize( int( vals.size() ) * int( sizeof( float ) ) );
  if ( !vals.isEmpty() )
    std::memcpy( raw.data(), vals.constData(), size_t( vals.size() ) * sizeof( float ) );
  return raw;
}

// TensorProto。dims 空 = 标量（[]）；raw float32 走 raw_data（字段 9）。
QByteArray tensorProto( const char *name, const QVector<int64_t> &dims, const QVector<float> &raw )
{
  Pb t;
  if ( !dims.isEmpty() )
    t.packedInt64Field( 1, dims );
  t.varintField( 2, 1 ); // data_type = FLOAT
  t.strField( 8, name );
  t.lenField( 9, rawFloatBytes( raw ) );
  return t.bytes;
}

// ValueInfoProto：name + tensor 类型 + 形状（dimValue<0 → 动态 dim_param，
// 按维位置命名 d<idx>——同一 value_info 内符号维同名会被形状推断解读为
// 「必须相等」，H/W 各自动态就要各自异名）。
QByteArray valueInfoProto( const char *name, const QVector<int64_t> &dims )
{
  Pb shape;
  for ( int i = 0; i < dims.size(); ++i )
  {
    Pb dim;
    if ( dims[i] >= 0 )
      dim.varintField( 1, uint64_t( dims[i] ) ); // Dimension.dim_value = 字段1
    else
    {
      const QByteArray param = QByteArray( "d" ) + QByteArray::number( i );
      dim.strField( 2, param.constData() ); // Dimension.dim_param = 字段2
    }
    shape.lenField( 1, dim.bytes );
  }
  Pb tensorType;
  tensorType.varintField( 1, 1 ); // elem_type = FLOAT
  tensorType.lenField( 2, shape.bytes );

  Pb type;
  type.lenField( 1, tensorType.bytes );

  Pb vi;
  vi.strField( 1, name );
  vi.lenField( 2, type.bytes );
  return vi.bytes;
}

struct NodeSpec
{
  const char *op = "";
  QVector<const char *> in, out;
};

QByteArray nodeProto( const NodeSpec &n )
{
  Pb p;
  for ( const char *i : n.in )
    p.strField( 1, i );
  for ( const char *o : n.out )
    p.strField( 2, o );
  p.strField( 4, n.op );
  return p.bytes;
}

struct GraphSpec
{
  const char *name = "g";
  QVector<NodeSpec> nodes;
  QVector<QByteArray> initializers; // 已编码的 TensorProto
  QVector<QPair<const char *, QVector<int64_t>>> inputs;  // (name, dims)
  QVector<QPair<const char *, QVector<int64_t>>> outputs;
};

QByteArray graphProto( const GraphSpec &g )
{
  Pb p;
  for ( const NodeSpec &n : g.nodes )
    p.lenField( 1, nodeProto( n ) );
  p.strField( 2, g.name );
  for ( const QByteArray &init : g.initializers )
    p.lenField( 5, init );
  for ( const auto &in : g.inputs )
    p.lenField( 11, valueInfoProto( in.first, in.second ) );
  for ( const auto &out : g.outputs )
    p.lenField( 12, valueInfoProto( out.first, out.second ) );
  return p.bytes;
}

QByteArray modelProto( int64_t irVersion, const QByteArray &graph )
{
  Pb opset;
  opset.strField( 1, "" ); // 默认域 ai.onnx
  opset.varintField( 2, 13 );

  Pb m;
  m.varintField( 1, uint64_t( irVersion ) );
  m.strField( 2, "paleo-fixture" );
  m.lenField( 7, graph );
  m.lenField( 8, opset.bytes );
  return m.bytes;
}

bool writeBytes( const QString &path, const QByteArray &bytes, QString *error )
{
  QFile f( path );
  if ( !f.open( QIODevice::WriteOnly ) )
  {
    if ( error )
      *error = QObject::tr( "cannot write fixture %1: %2" ).arg( path, f.errorString() );
    return false;
  }
  if ( f.write( bytes ) != bytes.size() )
  {
    if ( error )
      *error = QObject::tr( "short write for fixture %1" ).arg( path );
    return false;
  }
  f.close();
  return true;
}
} // namespace

bool OnnxFixtureWriter::writeAddScalar( const QString &path, float addend,
                                        int64_t irVersion, QString *error )
{
  GraphSpec g;
  g.name = "add_scalar";
  g.nodes.append( NodeSpec { "Add", { "x", "addend" }, { "y" } } );
  g.initializers.append( tensorProto( "addend", {}, { addend } ) );
  g.inputs.append( { "x", { 1 } } );
  g.outputs.append( { "y", { 1 } } );
  return writeBytes( path, modelProto( irVersion, graphProto( g ) ), error );
}

bool OnnxFixtureWriter::writeSeg( const QString &path, const QVector<float> &gains,
                                  const QVector<float> &biases, int64_t irVersion,
                                  QString *error )
{
  if ( gains.isEmpty() || gains.size() != biases.size() )
  {
    if ( error )
      *error = QObject::tr( "seg fixture needs equal non-empty gains/biases" );
    return false;
  }
  const int64_t c = gains.size();
  const QVector<int64_t> wbDims = { 1, c, 1, 1 };
  GraphSpec g;
  g.name = "seg_logits";
  g.nodes.append( NodeSpec { "Mul", { "x", "w" }, { "e" } } );
  g.nodes.append( NodeSpec { "Add", { "e", "b" }, { "y" } } );
  g.initializers.append( tensorProto( "w", wbDims, gains ) );
  g.initializers.append( tensorProto( "b", wbDims, biases ) );
  g.inputs.append( { "x", { 1, 1, -1, -1 } } );   // NCHW，H/W 动态
  g.outputs.append( { "y", { 1, c, -1, -1 } } );
  return writeBytes( path, modelProto( irVersion, graphProto( g ) ), error );
}

bool OnnxFixtureWriter::writeTraceScorer( const QString &path, float gain, float offset,
                                          int64_t irVersion, QString *error )
{
  GraphSpec g;
  g.name = "trace_scorer";
  g.nodes.append( NodeSpec { "Mul", { "t", "k" }, { "m" } } );
  g.nodes.append( NodeSpec { "Add", { "m", "o" }, { "a" } } );
  g.nodes.append( NodeSpec { "Sigmoid", { "a" }, { "p" } } );
  g.initializers.append( tensorProto( "k", {}, { gain } ) );
  g.initializers.append( tensorProto( "o", {}, { offset } ) );
  g.inputs.append( { "t", { 1, 1, -1 } } );
  g.outputs.append( { "p", { 1, 1, -1 } } );
  return writeBytes( path, modelProto( irVersion, graphProto( g ) ), error );
}
