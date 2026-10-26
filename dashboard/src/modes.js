const definitions = [
  ['cruise', 'Cruise'], ['hill', 'Hill assist'], ['autonomous', 'Autonomous'],
  ['remoteMode', 'Remote'], ['horn', 'Horn'], ['reverse', 'Reverse'],
]

export function dashboardModes(vehicle) {
  return definitions.map(([key, label]) => {
    const requested = key==='horn'?vehicle?.horn:vehicle?.switches?.[key]
    let state = requested === true ? 'ON' : requested === false ? 'OFF' : 'UNKNOWN'
    let title = `${label.toUpperCase()} ${requested ? 'ENABLED' : 'OFF'}`, detail = ''
    if (requested && key === 'cruise' && vehicle.cruiseActive !== true) {
      state = 'SELECTED'; title = 'CRUISE MODE SELECTED'; detail = 'Waiting for powertrain activation'
    }
    if (requested && key === 'hill') {
      state = 'UNAVAILABLE'; title = 'HILL ASSIST UNAVAILABLE'; detail = `Chassis pitch required · ${vehicle.powertrainReason===10?'pedal blocked · idle commanded':'waiting for powertrain acknowledgement'}`
    }
    if (requested && key === 'autonomous') {
      state = 'REQUESTED'; title = 'AUTONOMOUS REQUESTED'; detail = 'Autonomous control is not configured'
    }
    if (requested && key === 'horn') {
      title = 'HORN ON'; detail = 'Steering-wheel or website button · Node 1 D13 relay command'
    }
    if (requested && key === 'reverse' && vehicle.reverseActive !== true) {
      state = 'REQUESTED'; title = 'REVERSE REQUESTED'; detail = 'Waiting for applied direction'
    }
    if (key === 'remoteMode') title = `REMOTE MODE ${requested ? 'ENABLED' : 'OFF'}`
    if (typeof requested !== 'boolean') { title = `${label.toUpperCase()} STATUS UNAVAILABLE`; detail = key==='horn'?'Waiting for Node 1 relay status':'Waiting for switch status' }
    return { key, label, requested, state, title, detail }
  })
}
