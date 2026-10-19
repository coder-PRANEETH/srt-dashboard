// One press owns one lease. A stopped/failed gesture is never retried or resumed.
export function createSteeringClient(onState, fetcher = fetch) {
  let disposed = false, held = null, pollTimer, moveTimer, limitTimer, motionRequest, generation = 0
  let state = { ready: false, direction: null, pwm: null, held: 0, message: 'CONNECTING' }
  const publish = patch => { state = { ...state, ...patch }; if (!disposed) onState(state) }
  async function request(method, params, signal, keepalive = false) {
    const controller = new AbortController(), abort = () => controller.abort()
    signal?.addEventListener('abort', abort, { once: true })
    const timeout = setTimeout(abort, 250)
    try {
      const response = await fetcher('/api/steering', { method, cache: 'no-store', signal: controller.signal, keepalive,
        ...(method === 'POST' ? { headers: { 'X-SRT-Control': '1', 'Content-Type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams(params).toString() } : {}) })
      if (!response.ok) throw new Error(response.status === 409 ? 'RELEASE & RETRY' : 'CONTROL UNAVAILABLE')
      const data = await response.json()
      if (typeof data.ready !== 'boolean' || !Number.isInteger(data.token)) throw new Error('CONTROL UNAVAILABLE')
      return data
    } finally { clearTimeout(timeout); signal?.removeEventListener('abort', abort) }
  }
  async function stop(message = '') {
    const stoppedGeneration = ++generation; held = null; clearTimeout(moveTimer); clearTimeout(limitTimer); motionRequest?.abort()
    publish({ ready: false, held: 0, message })
    try {
      const data = await request('POST', { direction: 'stop' }, null, true)
      if (!held && generation === stoppedGeneration) publish({ ...data, message })
    } catch { if (!held && generation === stoppedGeneration) publish({ ready: false, direction: null, pwm: null, message: 'CONNECTION LOST' }) }
  }
  async function poll() {
    if (disposed) return
    const before = generation
    if (!held) {
      try {
        const data = await request('GET')
        if (before === generation && !held) publish({ ...data, message: data.ready ? '' : 'NODE 1 UNAVAILABLE' })
      } catch { if (before === generation && !held) publish({ ready: false, direction: null, pwm: null, message: 'CONNECTION LOST' }) }
    }
    if (!disposed) pollTimer = setTimeout(poll, 250)
  }
  async function move(gesture) {
    if (disposed || held !== gesture) return
    motionRequest = new AbortController()
    try {
      const data = await request('POST', { direction: gesture.direction === 1 ? 'left' : 'right', token: gesture.token, seq: ++gesture.sequence }, motionRequest.signal)
      if (held !== gesture || disposed) return
      if (!data.ready || data.token !== gesture.token) { stop('RELEASE & RETRY'); return }
      publish({ ...data, held: gesture.direction, message: '' })
      moveTimer = setTimeout(() => move(gesture), 80)
    } catch (error) { if (held === gesture && !disposed) stop(error.name === 'AbortError' ? 'CONNECTION LOST' : error.message) }
  }
  poll()
  return {
    start(direction) {
      if (held) { stop(); return false }
      if (disposed || !state.ready || state.direction !== 0 || ![1,2].includes(direction)) return false
      ++generation
      const gesture = { direction, token: state.token, sequence: 0 }; held = gesture
      publish({ held: direction, message: '' })
      limitTimer = setTimeout(() => stop('RELEASE & RETRY'), 4900)
      move(gesture); return true
    },
    stop,
    dispose() { if (disposed) return; disposed = true; clearTimeout(pollTimer); stop() },
  }
}
