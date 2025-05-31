# ThunderPlayer
ThunderPlayer é uma versão modificada do BlenderPlayer, originalmente desenvolvido pelo Blender Foundation. Este software é licenciado sob a GPLv2+.

# ThunderPlayer - UPBGE 0.2.5 com suporte a Compute Shaders (`.comp`)

**ThunderPlayer** é uma versão modificada do BlenderPlayer baseada no UPBGE 0.2.5, com o objetivo de integrar recursos modernos como **shaders de computação** (`.comp`) ao pipeline clássico da engine, além de outras funcionalidades planejadas.

---

## 🎯 Objetivo

Adicionar suporte real ao uso de **Compute Shaders (`.comp`)** dentro da engine UPBGE 0.2.5 para:

- Pré-processamento de dados diretamente na GPU.
- Geração procedural de texturas.
- Execução de algoritmos pesados (ex: Voronoi, simulações, física paralela).
- Comunicação com os shaders tradicionais (`.vert` e `.frag`) por meio de texturas.

---

## 🎯 Outros Objetivos

- Centralizar todo render .glsl em um único arquivo, e permitir sua edição e recompilação no editor de texto do blender/UPBGE.
- Atualizar a UI Visualmente.

---

## ⚙️ Método utilizado

Para tornar isso possível sem interferir no pipeline legacy principal, foi adotada a seguinte abordagem:

- Uma **janela OpenGL moderna invisível** é criada paralelamente à janela principal do jogo.
- Essa janela utiliza **OpenGL Core Profile**, permitindo a compilação e despacho de shaders `.comp` com `glDispatchCompute(...)`.
- Os dados processados no `.comp` podem ser armazenados em **texturas (via `imageStore`)** que futuramente serão lidos pelos shaders `.frag` e `.vert`.

Essa abordagem mantém a compatibilidade com o sistema legacy do UPBGE, enquanto habilita recursos modernos para desenvolvedores avançados.

---

## 📦 Estado atual

- ✅ Integração de um novo tipo de shader `.comp` via `setSource(...)`.
- ✅ Compilação e execução de shaders de computação.
- ✅ Uso de uma segunda janela invisível com contexto moderno para os `.comp`.
- ✅ A comunicação entre `.comp` e `.frag` **Funcional Com Bugs, perda da textura ao add uma cena overlay**.
- 🔧 Otimizações de desempenho ainda serão feitas para reduzir o tempo de carregamento e melhorar o gerenciamento de recursos.
- ⏳ Centralizar todo render .glsl em um único arquivo, e permitir sua edição e recompilação no editor de texto do blender/UPBGE.
- ⏳ Atualizar a UI Visualmente.

---

## 🚧 Funcionalidades futuras planejadas

Até o final de 2025, esperamos lançar uma versão **estável** do ThunderPlayer com os seguintes recursos adicionais:

- ✅ Suporte completo a Compute Shaders.
- 🔄 Comunicação eficiente entre `.comp`, `.frag` e `.vert`.
- 📹 Suporte a **vídeo playback** dentro do jogo.
- 🧵 Controle de **antisserrilhamento (anti-aliasing)** via script Python.
- ⚡ Otimizações gerais e estabilidade aumentada.

---

## 🎮 ThunderBlue Studios — Jogo em desenvolvimento

O estúdio **ThunderBlue Studios** também está desenvolvendo um jogo que será lançado usando essa engine modificada.

📌 **Resumo do jogo**:
- Gênero: Mundo aberto, exploração.
- Geração de terreno procedural com **seeds aleatórias**.
- Terreno que cresce dinamicamente com base na exploração do jogador.
- Desenvolvido inteiramente em **UPBGE modificado com suporte a `.comp`**.

---

## 🌟 Futuro: ThunderBlue Engine

Existe também a intenção de, no futuro, criar uma engine própria chamada **ThunderBlue Engine**, com foco em:
- Liberdade total ao desenvolvedor.
- Pipeline gráfico moderno.
- Compatibilidade com computação paralela e shaders avançados desde o núcleo.

---

## 👨‍💻 Sobre

Este projeto está sendo desenvolvido por **ThunderBlue Studios**, com foco em explorar o máximo potencial do UPBGE e do Blender Game Engine, trazendo ideias modernas para engines clássicas.
