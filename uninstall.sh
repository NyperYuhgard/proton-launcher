#!/bin/bash
# Proton Launcher - Uninstaller Script

set -e

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

check_root() {
    if [[ $EUID -ne 0 ]]; then
        print_error "Ejecuta como root: sudo ./uninstall.sh"
        exit 1
    fi
}

uninstall_files() {
    print_status "Desinstalando Proton Launcher..."
    
    # Remover aplicación principal
    rm -f /usr/bin/proton-launcher-gui
    
    # Remover herramientas opcionales
    rm -f /usr/local/bin/proton-ge
    rm -f /usr/local/bin/proton-ge-rtsp19
    
    # Remover directorio de configuración
    if [ -d "/etc/proton-launcher" ]; then
        rmdir "/etc/proton-launcher" 2>/dev/null || true
    fi
    
    print_status "Archivos removidos"
}

clean_user_files() {
    print_warning "¿Eliminar prefixes y configuraciones de usuario?"
    read -p "Esto borrará todos los datos de Wine (s/N): " -n 1 -r
    echo
    
    if [[ $REPLY =~ ^[Ss]$ ]]; then
        rm -rf "$HOME/.protonprefix"* 2>/dev/null || true
        rm -f "$HOME/.cache/proton-launcher.log" 2>/dev/null || true
        print_status "Datos de usuario eliminados"
    else
        print_status "Datos de usuario conservados"
    fi
}

verify_uninstall() {
    print_status "Verificando desinstalación..."
    
    local main_removed=true
    local tools=("proton-launcher-gui" "proton-ge" "proton-ge-rtsp19")
    
    for tool in "${tools[@]}"; do
        if command -v "$tool" >/dev/null 2>&1; then
            print_error "✗ $tool aún está instalado"
            main_removed=false
        else
            print_status "✓ $tool desinstalado"
        fi
    done
    
    if $main_removed; then
        print_status "¡Desinstalación completada!"
    else
        print_error "Algunos componentes no se pudieron desinstalar"
    fi
}

main() {
    echo "=========================================="
    echo "   Proton Launcher - Desinstalador"
    echo "=========================================="
    echo ""
    
    check_root
    uninstall_files
    clean_user_files
    verify_uninstall
}

main "$@"
