<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

<h1 align="center">
  <br>
  <b>brunoShadPs4</b>
  <br>
</h1>

<h1 align="center">
 <a href="https://github.com/serbru20066666/brunoShadPs4/stargazers">
        <img src="https://img.shields.io/github/stars/serbru20066666/brunoShadPs4" width="120">
 </a>
</h1>

# Información General

**brunoShadPs4** es un fork del emulador original [shadPS4](https://github.com/shadps4-emu/shadPS4), un emulador temprano de **PlayStation 4** para **Windows**, **Linux** y **macOS** escrito en C++.

Este fork está centrado en integrar mejoras gráficas, correcciones y ajustes específicos para juegos.

### Cambios integrados en este repositorio:
* **God of War III Remastered (Fix)**: Corrección de errores gráficos (líneas/rayas horizontales al renderizar) desactivando de manera forzada el MSAA del host específicamente para este título (CUSA01623 y CUSA01715).
* Ajustes de compilación mejorados y personalizados para SDL sin dependencias rotas de ramas anteriores.

* Antes:
*  <img width="1967" height="1248" alt="Captura de pantalla 2026-10-02 135217" src="https://github.com/user-attachments/assets/e0d7a8fa-25e6-4a53-8c75-39791790acaf" />

* Despues:
* <img width="1917" height="1090" alt="Captura de pantalla 2026-10-02 151146" src="https://github.com/user-attachments/assets/2cd0f1f5-4195-43a2-8e1d-ba74e3e4d6ac" />




> [!IMPORTANT]
> Este repositorio contiene principalmente el núcleo del emulador (core). Para compilarlo, es posible que necesites las dependencias de Vulkan y SDL3 configuradas adecuadamente en tu entorno.

# Estado
El emulador (y este fork) sigue en etapa de desarrollo activo.

# Licencia
El código original de este proyecto y sus modificaciones están bajo licencia de código abierto de acuerdo al proyecto original.
