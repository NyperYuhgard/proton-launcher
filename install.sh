#!/bin/bash
# Proton Launcher - Installer Script
# Autor: Nyper Yuhgard

set -e

# Colores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

print_status() {
    echo -e "${GREEN}[+]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[!]${NC} $1"
}

print_error() {
    echo -e "${RED}[!]${NC} $1"
}

# Obtener el usuario original que ejecutó sudo
get_original_user() {
    if [ -n "$SUDO_USER" ]; then
        echo "$SUDO_USER"
    else
        echo "$(whoami)"
    fi
}

# Obtener el home del usuario original
get_original_home() {
    local user="$1"
    if [ "$user" = "root" ]; then
        echo "/root"
    else
        echo "/home/$user"
    fi
}

check_root() {
    if [[ $EUID -eq 0 ]]; then
        print_status "Ejecutando como root"
    else
        print_error "Este script debe ejecutarse como root para instalar en /usr/bin"
        print_error "Usa: sudo ./install.sh"
        exit 1
    fi
}

check_dependencies() {
    local deps=("zenity" "wget")
    local missing=()
    
    for dep in "${deps[@]}"; do
        if ! command -v "$dep" >/dev/null 2>&1; then
            missing+=("$dep")
        fi
    done
    
    if [ ${#missing[@]} -gt 0 ]; then
        print_warning "Dependencias faltantes: ${missing[*]}"
        read -p "¿Instalar dependencias? (s/N): " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Ss]$ ]]; then
            if command -v apt >/dev/null 2>&1; then
                apt update && apt install -y "${missing[@]}"
            elif command -v dnf >/dev/null 2>&1; then
                dnf install -y "${missing[@]}"
            elif command -v pacman >/dev/null 2>&1; then
                pacman -Sy --noconfirm "${missing[@]}"
            else
                print_error "Instala manualmente: ${missing[*]}"
            fi
        fi
    fi
}

install_main_app() {
    print_status "Instalando Proton Launcher GUI en /usr/bin..."
    install -m 755 usr/bin/proton-launcher-gui /usr/bin/proton-launcher-gui
}

install_optional_tools() {
    print_status "Instalando herramientas opcionales..."
    
    if [ -f "usr/local/bin/proton-ge" ]; then
        install -m 755 usr/local/bin/proton-ge /usr/local/bin/proton-ge
        print_status "✓ proton-ge instalado"
    else
        print_warning "proton-ge no encontrado en el paquete"
    fi
    
    if [ -f "usr/local/bin/proton-ge-rtsp19" ]; then
        install -m 755 usr/local/bin/proton-ge-rtsp19 /usr/local/bin/proton-ge-rtsp19
        print_status "✓ proton-ge-rtsp19 instalado"
    else
        print_warning "proton-ge-rtsp19 no encontrado en el paquete"
    fi
}

create_user_dirs() {
    local original_user=$(get_original_user)
    local user_home=$(get_original_home "$original_user")
    
    print_status "Creando directorios para el usuario: $original_user"
    
    # Crear directorio Proton-Files en el home del usuario
    local proton_dir="$user_home/Proton-Files"
    mkdir -p "$proton_dir"
    chown "$original_user:$original_user" "$proton_dir"
    chmod 755 "$proton_dir"
    
    # Crear directorio .cache si no existe
    local cache_dir="$user_home/.cache"
    mkdir -p "$cache_dir"
    chown "$original_user:$original_user" "$cache_dir"
    
    print_status "Directorio Proton creado: $proton_dir"
    
    # Mostrar mensaje de ayuda para el usuario
    echo ""
    print_warning "IMPORTANTE: Coloca tus versiones de Proton-GE en:"
    print_warning "  $proton_dir"
    echo ""
}

verify_installation() {
    print_status "Verificando instalación..."
    
    # Verificar instalación principal
    if command -v proton-launcher-gui >/dev/null 2>&1; then
        print_status "✓ proton-launcher-gui instalado en /usr/bin"
    else
        print_error "✗ proton-launcher-gui no se pudo instalar"
        exit 1
    fi
    
    # Verificar herramientas opcionales
    local optional_tools=("proton-ge" "proton-ge-rtsp19")
    for tool in "${optional_tools[@]}"; do
        if command -v "$tool" >/dev/null 2>&1; then
            print_status "✓ $tool instalado en /usr/local/bin"
        else
            print_warning "⚠ $tool no instalado (opcional) en desuso"
        fi
    done
    
    show_usage
}

show_usage() {
    local original_user=$(get_original_user)
    local user_home=$(get_original_home "$original_user")
    
    echo ""
    print_status "¡Instalación completada!"
    echo ""
    print_status "Uso principal:"
    echo "  proton-launcher-gui    # Interfaz gráfica completa"
    echo ""
    print_status "Herramientas opcionales:"
    echo "  proton-ge <ejecutable>      # Ejecutar con GE-Proton estándar"
    echo "  proton-ge-rtsp19 <ejecutable> # Ejecutar con GE-Proton RTSP19"
    echo ""
    print_warning "Coloca Proton-GE en: $user_home/Proton-Files/"
    echo ""
    print_status "Ejemplo de uso:"
    echo "  $ proton-launcher-gui"
    echo ""
}

main() {
    echo "=========================================="
    echo "    Proton Launcher - Instalador"
    echo "=========================================="
    echo ""
    
    local original_user=$(get_original_user)
    print_status "Instalando para el usuario: $original_user"
    echo ""
    
    check_root
    check_dependencies
    install_main_app
  #  install_optional_tools (Now Unused)
    create_user_dirs
    verify_installation
}

main "$@"
