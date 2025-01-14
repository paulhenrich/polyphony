# frozen_string_literal: true

require_relative 'helper'

class BackendTest < MiniTest::Test
  def setup
    super
    @prev_backend = Thread.current.backend
    @backend = Polyphony::Backend.new
    Thread.current.backend = @backend
  end

  def teardown
    @backend.finalize
    Thread.current.backend = @prev_backend
  end

  def test_sleep
    count = 0
    t0 = monotonic_clock
    spin {
      @backend.sleep 0.01
      count += 1
      @backend.sleep 0.01
      count += 1
      @backend.sleep 0.01
      count += 1
    }.await
    assert_in_range 0.02..0.06, monotonic_clock - t0 if IS_LINUX
    assert_equal 3, count
  end

  def test_write_read_partial
    i, o = IO.pipe
    buf = +''
    f = spin { @backend.read(i, buf, 5, false, 0) }
    @backend.write(o, 'Hello world')
    return_value = f.await

    assert_equal 'Hello', buf
    assert_equal return_value, buf
  end

  def test_write_read_to_eof_limited_buffer
    i, o = IO.pipe
    buf = +''
    f = spin { @backend.read(i, buf, 5, true, 0) }
    @backend.write(o, 'Hello')
    snooze
    @backend.write(o, ' world')
    snooze
    o.close
    return_value = f.await

    assert_equal 'Hello', buf
    assert_equal return_value, buf
  end

  def test_write_read_to_eof
    i, o = IO.pipe
    buf = +''
    f = spin { @backend.read(i, buf, 10**6, true, 0) }
    @backend.write(o, 'Hello')
    snooze
    @backend.write(o, ' world')
    snooze
    o.close
    return_value = f.await

    assert_equal 'Hello world', buf
    assert_equal return_value, buf
  end

  def test_write_read_with_buffer_position
    i, o = IO.pipe
    buf = +'abcdefghij'
    f = spin { @backend.read(i, buf, 20, false, 0) }
    @backend.write(o, 'blah')
    return_value = f.await

    assert_equal 'blah', buf
    assert_equal return_value, buf

    buf = +'abcdefghij'
    f = spin { @backend.read(i, buf, 20, false, -1) }
    @backend.write(o, 'klmn')
    return_value = f.await

    assert_equal 'abcdefghijklmn', buf
    assert_equal return_value, buf

    buf = +'abcdefghij'
    f = spin { @backend.read(i, buf, 20, false, 3) }
    @backend.write(o, 'DEF')
    return_value = f.await

    assert_equal 'abcDEF', buf
    assert_equal return_value, buf
  end

  def test_read_concat_big
    i, o = IO.pipe

    body = " " * 4000

    data = "post /?q=time&blah=blah HTTP/1\r\nHost: dev.realiteq.net\r\n\r\n" +
           "get /?q=time HTTP/1.1\r\nContent-Length: #{body.bytesize}\r\n\r\n#{body}" +
           "get /?q=time HTTP/1.1\r\nCookie: foo\r\nCookie: bar\r\n\r\n"

    o << data
    o.close

    buf = +''

    @backend.read(i, buf, 4096, false, -1)
    assert_equal 4096, buf.bytesize

    @backend.read(i, buf, 1, false, -1)
    assert_equal 4097, buf.bytesize

    @backend.read(i, buf, 4096, false, -1)

    assert_equal data.bytesize, buf.bytesize
    assert_equal data, buf
  end

  def test_waitpid
    pid = fork do
      @backend.post_fork
      exit(42)
    end

    result = @backend.waitpid(pid)
    assert_equal [pid, 42], result
  end

  def test_read_loop
    i, o = IO.pipe

    buf = []
    f = spin do
      buf << :ready
      @backend.read_loop(i, 8192) { |d| buf << d }
      buf << :done
    end

    # Caution: we want to read in two chunks, that a race condition lurks here:
    # writing always causes snoozing, but that might not be enough for the
    # reader fiber to perform the read before the second write, so for the test
    # to work consistently we add a little sleep between the two writes.
    o << 'foo'
    sleep 0.03
    o << 'bar'
    o.close

    f.await
    assert_equal [:ready, 'foo', 'bar', :done], buf
  end

  def test_read_loop_terminate
    i, o = IO.pipe

    buf = []
    parent = spin do
      f = spin do
        buf << :ready
        @backend.read_loop(i, 8192) { |d| buf << d }
        buf << :done
      end
      suspend
    end

    # writing always causes snoozing
    o << 'foo'
    sleep 0.01
    o << 'bar'
    sleep 0.01

    parent.stop

    parent.await
    assert_equal [:ready, 'foo', 'bar'], buf
  end

  def test_read_loop_with_max_len
    r, w = IO.pipe

    w << 'foobar'
    w.close
    buf = []
    @backend.read_loop(r, 3) { |data| buf << data }
    assert_equal ['foo', 'bar'], buf
  end

  Net = Polyphony::Net

  def test_accept
    port = rand(1234..4321)

    server = Net.send(
      :listening_socket_from_options, '127.0.0.1', port, reuse_addr: true
    )

    clients = []
    server_fiber = spin_loop do
      c = @backend.accept(server, TCPSocket)
      clients << c
    end

    c1 = TCPSocket.new('127.0.0.1', port)
    sleep 0.01

    assert_equal 1, clients.size

    c2 = TCPSocket.new('127.0.0.1', port)
    sleep 0.01

    assert_equal 2, clients.size

  ensure
    c1&.close
    c2&.close
    server_fiber&.stop
    snooze
    server&.close
  end

  def test_accept_loop
    port = rand(1234..4321)
    server = Net.send(
      :listening_socket_from_options, '127.0.0.1', port, reuse_addr: true
    )

    clients = []
    server_fiber = spin do
      @backend.accept_loop(server, TCPSocket) { |c| clients << c }
    end

    c1 = TCPSocket.new('127.0.0.1', port)
    sleep 0.01

    assert_equal 1, clients.size

    c2 = TCPSocket.new('127.0.0.1', port)
    sleep 0.01

    assert_equal 2, clients.size
  ensure
    c1&.close
    c2&.close
    server_fiber&.stop
    snooze
    server&.close
  end

  def test_timer_loop
    counter = 0
    f = spin do
      @backend.timer_loop(0.01) { counter += 1 }
    end
    @backend.sleep(0.05)
    f.stop
    f.await # TODO: check why this test sometimes segfaults if we don't a<wait fiber
    assert_in_range 3..6, counter if IS_LINUX
  end

  class MyTimeoutException < Exception
  end

  def test_timeout
    skip unless IS_LINUX

    buffer = []
    assert_raises(Polyphony::TimeoutException) do
      @backend.timeout(0.01, Polyphony::TimeoutException) do
        buffer << 1
        sleep 0.02
        buffer << 2
      end
    end
    assert_equal [1], buffer

    buffer = []
    assert_raises(MyTimeoutException) do
      @backend.timeout(0.01, MyTimeoutException) do
        buffer << 1
        sleep 1
        buffer << 2
      end
    end
    assert_equal [1], buffer

    buffer = []
    result = @backend.timeout(0.01, nil, 42) do
      buffer << 1
      sleep 1
      buffer << 2
    end
    assert_equal 42, result
    assert_equal [1], buffer
  end

  def test_nested_timeout
    skip unless IS_LINUX

    buffer = []
    assert_raises(MyTimeoutException) do
      @backend.timeout(0.01, MyTimeoutException) do
        @backend.timeout(0.02, nil) do
          buffer << 1
          sleep 1
          buffer << 2
        end
      end
    end
    assert_equal [1], buffer
  end

  def test_splice_chunks
    body = 'abcd' * 4
    chunk_size = 12

    buf = +''
    r, w = IO.pipe
    reader = spin do
      r.read_loop { |data| buf << data }
    end

    i, o = IO.pipe
    writer = spin do
      o << body
      o.close
    end
    Thread.current.backend.splice_chunks(
      i,
      w,
      "Content-Type: foo\r\n\r\n",
      "0\r\n\r\n",
      ->(len) { "#{len.to_s(16)}\r\n" },
      "\r\n",
      chunk_size
    )
    w.close
    reader.await

    expected = "Content-Type: foo\r\n\r\n#{12.to_s(16)}\r\n#{body[0..11]}\r\n#{4.to_s(16)}\r\n#{body[12..15]}\r\n0\r\n\r\n"
    assert_equal expected, buf
  ensure
    o.close
    w.close
  end

  def test_idle_gc
    GC.disable

    count = GC.count
    snooze
    assert_equal count, GC.count
    sleep 0.01
    assert_equal count, GC.count

    @backend.idle_gc_period = 0.1
    snooze
    assert_equal count, GC.count
    sleep 0.05
    assert_equal count, GC.count

    return unless IS_LINUX

    # The idle tasks are ran at most once per fiber switch, before the backend
    # is polled. Therefore, the second sleep will not have triggered a GC, since
    # only 0.05s have passed since the gc period was set.
    sleep 0.07
    assert_equal count, GC.count
    # Upon the third sleep the GC should be triggered, at 0.12s post setting the
    # GC period.
    sleep 0.05
    assert_equal count + 1, GC.count

    @backend.idle_gc_period = 0
    count = GC.count
    sleep 0.001
    sleep 0.002
    sleep 0.003
    assert_equal count, GC.count
  ensure
    GC.enable
  end

  def test_idle_proc
    counter = 0

    @backend.idle_proc = proc { counter += 1 }

    3.times { snooze }
    assert_equal 0, counter

    sleep 0.01
    assert_equal 1, counter
    sleep 0.01
    assert_equal 2, counter

    assert_equal 2, counter
    3.times { snooze }
    assert_equal 2, counter

    @backend.idle_proc = nil
    sleep 0.01
    assert_equal 2, counter
  end

  def test_read_write_with_io_buffer
    skip "Works only on Ruby >= 3.1" if RUBY_VERSION < '3.1'

    msg = 'Hello world'
    i, o = IO.pipe
    read_buffer = IO::Buffer.new(64)
    f = spin { @backend.read(i, read_buffer, 64, true, 0) }
    write_buffer = IO::Buffer.new(msg.bytesize)
    write_buffer.set_string(msg, 0)
    @backend.write(o, write_buffer)
    o.close
    return_value = f.await

    assert_equal msg.bytesize, return_value
    assert_equal msg, read_buffer.get_string(0, msg.bytesize)
  end
end

class BackendChainTest < MiniTest::Test
  def setup
    super
    @prev_backend = Thread.current.backend
    @backend = Polyphony::Backend.new
    Thread.current.backend = @backend
  end

  def teardown
    @backend.finalize
    Thread.current.backend = @prev_backend
  end

  def test_simple_write_chain
    i, o = IO.pipe

    result = Thread.backend.chain(
      [:write, o, 'hello'],
      [:write, o, ' world']
    )

    assert_equal 6, result
    o.close
    assert_equal 'hello world', i.read
  end

  def test_simple_send_chain
    port = rand(1234..5678)
    server = TCPServer.new('127.0.0.1', port)

    server_fiber = spin do
      while (socket = server.accept)
        spin do
          while (data = socket.gets(8192))
            socket << data
          end
        end
      end
    end

    snooze
    client = TCPSocket.new('127.0.0.1', port)

    result = Thread.backend.chain(
      [:send, client, 'hello', 0],
      [:send, client, " world\n", 0]
    )
    sleep 0.1
    assert_equal "hello world\n", client.recv(8192)
    client.close
  ensure
    server_fiber&.stop
    server_fiber&.await
    server&.close
  end

  def chunk_header(len)
    "Content-Length: #{len}\r\n\r\n"
  end

  def serve_io(from, to)
    i, o = IO.pipe
    backend = Thread.current.backend
    while true
      len = o.splice_from(from, 8192)
      break if len == 0

      backend.chain(
        [:write, to, chunk_header(len)],
        [:splice, i, to, len]
      )
    end
    to.close
  end

  def test_chain_with_splice
    from_r, from_w = IO.pipe
    to_r, to_w = IO.pipe

    result = nil
    f = spin { serve_io(from_r, to_w) }

    from_w << 'Hello world!'
    from_w.close

    assert_equal "Content-Length: 12\r\n\r\nHello world!", to_r.read
  end

  def test_invalid_op
    i, o = IO.pipe

    assert_raises(RuntimeError) {
      Thread.backend.chain(
        [:read, i]
      )
    }

    assert_raises(RuntimeError) {
      Thread.backend.chain(
        [:write, o, 'abc'],
        [:write, o, 'abc'],
        [:write, o, 'abc'],
        [:read, i]
      )
    }

    assert_raises(RuntimeError) {
      Thread.backend.chain(
        [:write, o]
      )
    }

    # Eventually we should add some APIs to the io_uring backend to query the
    # contxt store, then add some tests here to verify that the chain op ctx is
    # released properly before raising the error (for the time being this has
    # been verified manually).
  end
end
