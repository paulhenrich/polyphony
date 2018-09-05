#!/usr/bin/env ruby
# frozen_string_literal: true

require 'modulation'

Net =     import('../../lib/nuclear/net')
Reactor = import('../../lib/nuclear/reactor')
include   import('../../lib/nuclear/concurrency')

socket = Net::Socket.new

async do
  await socket.connect('127.0.0.1', 1234, timeout: 3)

  timer_id = Reactor.interval(1) { socket << "#{Time.now}\n" }
  Reactor.timeout(5) do
    Reactor.cancel_timer(timer_id)
    socket.close
  end

  while data = await(socket.read) do
    STDOUT << data
  end
end