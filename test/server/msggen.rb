#!/usr/bin/env ruby

@test_data = [
              "OML000000420000000CHello World",
              "OML000000430000000CHellp World",
              "OML000000440000000CHellq World",
              "OML000000450000000CHellr World",
              "OML000000460000000CHells World",
              "OML000000470000000CHellt World",
              "OML000000480000000CHellr World",
              "OML000000490000000CHellv World",
              "OML0000004A0000000CHellw World",
              "OML000000420000000CHellx World",
              "OML000000420000000CHelly World",
              "OML000000420000000CHellz World",
             ]

def serialize_text(value)
  value.to_s
end

def serialize(type_, value, content)
  if content == "text"
    serialize_text(value)
  end
end

class Client
  def initialize(sender, experiment, application, start_time)
    @sender = sender
    @experiment = experiment
    @application = application
    @start_time = start_time
    @streams = []
  end

  # schema should be a hash (name => type)
  def defStream(index, name, schema)
    @streams << Stream.new(index,name,schema)
  end

  def headers(content)
    h = []
    h << "protocol: 2"
    h << "experiment-id: #{@experiment}"
    h << "start_time: #{@start_time}"
    h << "sender-id: #{@sender}"
    h << "app-name: #{@application}"
    h = h + @streams.collect { |stream| stream.schema_string }
    h << "content: #{content}"
  end

  def gen(index, content)
    if index < 1 || index > @streams.length
      return
    end

    sample = @streams[index-1].gen

    if content == "text" then
      header = ["#{sample[:timestamp]}",
                "#{sample[:index]}",
                "#{sample[:seqno]}"].join("\t")
      values = @streams[index-1].schema.each_value.zip(sample[:measurement])
      measurement = values.each.collect { |t,v| serialize(t, v, content) }
      header + "\t" + measurement.join("\t") + "\n"
    end
  end
end

class Stream
  attr_reader :schema

  def initialize(index, name, schema)
    @index = index
    @name = name
    @schema = schema
    @generators = @schema.each_value.collect { |v| Generator.new(v) }
    @seqno = 0
    @timestamp = 0.0
  end

  def schema_string
    field_specs = @schema.each_pair.collect { |n, t| "#{n}:#{t}" }.join(" ")
    @seqno += 1
    "schema: #{@name} #{@index} #{field_specs}"
  end

  def gen
    @seqno += 1
    @timestamp += rand
    { :index => @index,
      :seqno => @seqno,
      :timestamp => @timestamp,
      :measurement => @generators.collect { |g| g.gen } }
  end
end

def random_string(n)
  length = Math.sqrt(rand((n/2)**2)).to_i # Maybe not optimal
  length.times.collect { (rand(26) + 65).chr }.join
end

class Generator
  def initialize(oml_type)
    @type = oml_type
  end

  def gen
    val = case @type
          when :long, :int32 then rand(1<<32) - (1<<31)
          when :int64  then rand(1<<64) - (1<<63)
          when :uint32 then rand(1<<32)
          when :uint64 then rand(1<<64)
          when :double then rand
          when :string then random_string(254)
          end
    case @type
    when :long, :int32 then
      if val == (1<<32) then
        val = (1<<32)-1
      end
    when :int64 then
      if val == (1<<64) then
        val = (1<<64)-1
      end
    end
    val
  end

  def get(&block)
    yield gen
  end
end

def make_packet(seqno, string)
  ("OML%08x%08x" % [seqno, string.length]) + string
end

def run
  c = Client.new("a", "ae", "app", 1234556)
  c.defStream(1, "generator_lin", "label" => :string, "seq_no" => :long)
  c.defStream(2, "generator_sin", "label" => :string, "phase" => :double, "angle" => :double)

  headers = c.headers("text")
  packets = []
  10.times { packets << c.gen(1, "text") }

  io = IO.popen("./msgloop", "w+")

#  @test_data.each { |msg|
#    io.puts msg
#  }

  seqno = 0
  headers.each { |h| io.write(make_packet(seqno, h + "\n")); seqno += 1 }
  io.write(make_packet(seqno, "\n")) # Headers/content separator.
  packets.each { |p| io.write(make_packet(seqno, p)); seqno += 1}
#  packets.each { |pack| p make_packet(seqno, pack); seqno +=1  }

  puts "Wrote all test data to msgloop pipe"

  io.close_write
  puts "Reading from pipe"
  io.readlines.each { |line| puts line }
  puts "Closed pipe; exiting"
  exit 0
end

if __FILE__ == $PROGRAM_NAME then run; end
