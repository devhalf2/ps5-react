import {useState} from 'react';
import {AppRegistry, View, Text, FocusScope, ShaderView, BootIntro} from '@ps5-react/core';
import {useInterfaceSounds} from './sounds.js';

// Rising embers: one particle per cell of a scrolling grid, three cells searched per pixel.
const embers = `
uniform float uTime;
uniform vec2 uResolution;
uniform vec4 uColor;
uniform float uSpeed;
out vec4 fragColor;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main() {
  vec2 uv = gl_FragCoord.xy / uResolution.y;
  vec3 col = mix(vec3(0.02, 0.03, 0.06), vec3(0.07, 0.05, 0.10), uv.y);
  for (int layer = 0; layer < 3; layer++) {
    float cell = 0.05 + 0.035 * float(layer);
    vec2 g = uv / cell - vec2(0.0, uTime * uSpeed * (0.6 + 0.4 * float(layer)) / cell * 0.1);
    for (int i = -1; i <= 1; i++) {
      vec2 id = floor(g) + vec2(i, 0.0);
      float h = hash(id + float(layer) * 17.0);
      if (h > 0.35) continue;
      vec2 p = id + vec2(0.5 + 0.35 * sin(uTime + h * 40.0), fract(h * 9.1));
      float d = length(g - p) * cell * uResolution.y;
      float r = 1.5 + 2.5 * float(layer);
      col += uColor.rgb * uColor.a * exp(-d * d / (r * r)) * (0.5 + 0.5 * sin(uTime * 3.0 + h * 30.0));
    }
  }
  fragColor = vec4(col, 1.0);
}
`;

const label = 'font-inter text-body text-white';

function Logo() {
  return (
    <View className="items-center gap-3">
      <Text className="font-inter text-logo text-white">Shader Lab</Text>
      <Text className="font-inter text-hint text-mist">PS5 React</Text>
    </View>
  );
}

function Particles() {
  return (
    <View className="w-screen h-screen bg-ink items-center justify-center">
      <View className="w-[960px] h-[540px] rounded-xl bg-panel p-6 gap-4">
        <Text className={label}>A ShaderView inside a panel, with UI above it</Text>
        <ShaderView source={embers} uniforms={{uColor: '#ffb070', uSpeed: 0.6}}
          className="flex-1 items-center justify-center">
          <View className="rounded-xl bg-ink/70 px-6 py-3">
            <Text className={label}>Embers on the GPU</Text>
          </View>
        </ShaderView>
        <Text className="font-inter text-hint text-accent">Circle / Backspace: back</Text>
      </View>
    </View>
  );
}

function Menu({onPick}) {
  return (
    <View className="w-screen h-screen bg-ink items-center justify-center gap-8">
      <Text className="font-inter text-title text-white">Shader Lab</Text>
      <View className="flex-row gap-4">
        {[['Replay intro', 'intro'], ['Particles', 'particles']].map(([text, target]) => (
          <View key={target} onPress={() => onPick(target)} className="w-[260px] h-[92px] rounded-xl
            border-[3px] border-edge bg-panel items-center justify-center
            focused:border-accent focused:bg-panel-focus focused:scale-105">
            <Text className={label}>{text}</Text>
          </View>
        ))}
      </View>
      <Text className="font-inter text-hint text-accent">D-pad: move    X / Enter: select</Text>
    </View>
  );
}

function App() {
  const [page, setPage] = useState('menu');
  const [intro, setIntro] = useState(0);
  useInterfaceSounds();
  const pick = target => {
    if (target === 'intro') setIntro(n => n + 1);
    else setPage(target);
  };
  // The intro plays at launch and on replay; the menu crosses over its last frames.
  return (
    <BootIntro key={intro} logo={<Logo />}>
      <FocusScope autoFocus wrap onBack={() => { setPage('menu'); return true; }}>
        {page === 'particles' ? <Particles /> : <Menu onPick={pick} />}
      </FocusScope>
    </BootIntro>
  );
}

AppRegistry.registerComponent('shader-lab', () => App);
