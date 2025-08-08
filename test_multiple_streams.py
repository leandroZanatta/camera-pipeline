#!/usr/bin/env python3
"""
Teste de múltiplos streams simultâneos.
Este teste cria 20 câmeras com a mesma URL para avaliar escalabilidade.
"""

import time
import threading
import psutil
import os
from camera_pipeline.core.processor import CameraProcessor
from camera_pipeline.core.callbacks import SimpleFrameCallback, SimpleStatusCallback

# Removido: configuração de logging do Python para eliminar logs

class MultiStreamCallback:
    """Callback para múltiplos streams"""
    
    def __init__(self):
        self.frame_counts = {}  # camera_id -> count
        self.status_updates = {}  # camera_id -> status
        self.lock = threading.Lock()
        self.start_time = time.time()
        
    def process_frame(self, camera_id, frame_data):
        """Processa frame de qualquer câmera"""
        with self.lock:
            if camera_id not in self.frame_counts:
                self.frame_counts[camera_id] = 0
            self.frame_counts[camera_id] += 1
            
            # Log apenas a cada 10 frames para reduzir spam
            if self.frame_counts[camera_id] % 10 == 0:
                elapsed = time.time() - self.start_time
                fps = self.frame_counts[camera_id] / elapsed if elapsed > 0 else 0
                print(f"[CÂMERA {camera_id:2d}] Frame {self.frame_counts[camera_id]:3d} - FPS: {fps:.2f}")
    
    def update_status(self, camera_id, status_code, message):
        """Atualiza status de qualquer câmera"""
        with self.lock:
            self.status_updates[camera_id] = {
                'status': status_code,
                'message': message,
                'timestamp': time.time()
            }
            
            # Log apenas mudanças importantes
            if status_code in [2, 0]:  # Conectado ou Parado
                print(f"[STATUS] Câmera {camera_id:2d}: {status_code} - {message}")
    
    def get_stats(self):
        """Retorna estatísticas de todas as câmeras"""
        with self.lock:
            total_frames = sum(self.frame_counts.values())
            active_cameras = len([c for c in self.status_updates.values() if c['status'] == 2])
            elapsed = time.time() - self.start_time
            
            return {
                'total_frames': total_frames,
                'active_cameras': active_cameras,
                'total_cameras': len(self.frame_counts),
                'elapsed_time': elapsed,
                'total_fps': total_frames / elapsed if elapsed > 0 else 0,
                'avg_fps_per_camera': total_frames / len(self.frame_counts) / elapsed if len(self.frame_counts) > 0 and elapsed > 0 else 0
            }

def get_system_stats():
    """Obtém estatísticas do sistema"""
    process = psutil.Process(os.getpid())
    memory_info = process.memory_info()
    
    return {
        'cpu_percent': process.cpu_percent(),
        'memory_mb': memory_info.rss / 1024 / 1024,
        'threads': process.num_threads(),
        'open_files': len(process.open_files()),
        'connections': len(process.connections())
    }

def main():
    """Função principal do teste"""
    print("=" * 80)
    print("TESTE DE MÚLTIPLOS STREAMS - 10 CÂMERAS SIMULTÂNEAS")
    print("=" * 80)
    
    # URL de teste
    test_url = "https://connect-162.servicestream.io:8050/cb5c5ee1a832.m3u8"
    num_cameras = 10
    
    try:
        # Criar processador
        print(f"\n[1] Criando processador para {num_cameras} câmeras...")
        processor = CameraProcessor(c_log_level=2)  # INFO para ver logs de inicialização
        
        # Inicializar biblioteca C
        print("\n[2] Inicializando biblioteca C...")
        if not processor.initialize_c_library():
            print("[ERRO] Falha ao inicializar biblioteca C")
            return False
        
        # Criar callback compartilhado
        print("\n[3] Criando callback compartilhado...")
        shared_callback = MultiStreamCallback()
        status_cb = SimpleStatusCallback(shared_callback.update_status)
        frame_cb = SimpleFrameCallback(shared_callback.process_frame)
        
        # Registrar múltiplas câmeras
        print(f"\n[4] Registrando {num_cameras} câmeras...")
        successful_registrations = 0
        
        for camera_id in range(1, num_cameras + 1):
            ret = processor.register_camera(
                camera_id=camera_id,
                url=test_url,
                frame_callback=frame_cb,
                status_callback=status_cb,
                target_fps=2  # 5 FPS para avaliar melhor throughput
            )
            
            if ret == 0:
                successful_registrations += 1
                if camera_id % 10 == 0:
                    print(f"   Registradas {camera_id} câmeras...")
            else:
                print(f"   [ERRO] Falha ao registrar câmera {camera_id}: código {ret}")
        
        print(f"[SUCESSO] {successful_registrations}/{num_cameras} câmeras registradas")
        
        if successful_registrations == 0:
            print("[ERRO] Nenhuma câmera foi registrada com sucesso")
            return False
        
        # Monitorar por 60 segundos
        print(f"\n[5] Monitorando {successful_registrations} câmeras por 60 segundos...")
        print("Avaliando escalabilidade e performance do sistema...")
        
        start_time = time.time()
        last_stats_time = start_time
        
        while time.time() - start_time < 60:
            time.sleep(10)  # Relatório a cada 10 segundos
            
            current_time = time.time()
            elapsed = current_time - start_time
            
            # Estatísticas do sistema
            sys_stats = get_system_stats()
            
            # Estatísticas das câmeras
            cam_stats = shared_callback.get_stats()
            
            print(f"\n📊 RELATÓRIO {elapsed:.0f}s:")
            print(f"   Câmeras ativas: {cam_stats['active_cameras']}/{cam_stats['total_cameras']}")
            print(f"   Total de frames: {cam_stats['total_frames']}")
            print(f"   FPS total: {cam_stats['total_fps']:.2f}")
            print(f"   FPS médio por câmera: {cam_stats['avg_fps_per_camera']:.2f}")
            print(f"   CPU: {sys_stats['cpu_percent']:.1f}%")
            print(f"   Memória: {sys_stats['memory_mb']:.1f} MB")
            print(f"   Threads: {sys_stats['threads']}")
            print(f"   Conexões: {sys_stats['connections']}")
            
            # Verificar se ainda há câmeras ativas
            if cam_stats['active_cameras'] == 0:
                print(f"[AVISO] Nenhuma câmera ativa após {elapsed:.0f}s")
                break
        
        # Análise final
        print(f"\n[6] Análise final...")
        final_stats = shared_callback.get_stats()
        final_sys_stats = get_system_stats()
        
        print(f"\n📈 RESULTADOS FINAIS:")
        print(f"   Tempo total: {final_stats['elapsed_time']:.1f}s")
        print(f"   Câmeras registradas: {final_stats['total_cameras']}")
        print(f"   Câmeras ativas: {final_stats['active_cameras']}")
        print(f"   Total de frames: {final_stats['total_frames']}")
        print(f"   FPS total: {final_stats['total_fps']:.2f}")
        print(f"   FPS médio por câmera: {final_stats['avg_fps_per_camera']:.2f}")
        print(f"   CPU final: {final_sys_stats['cpu_percent']:.1f}%")
        print(f"   Memória final: {final_sys_stats['memory_mb']:.1f} MB")
        print(f"   Threads finais: {final_sys_stats['threads']}")
        print(f"   Conexões finais: {final_sys_stats['connections']}")
        
        # Interpretação
        print(f"\n🔍 INTERPRETAÇÃO:")
        
        # Avaliar escalabilidade
        if final_stats['active_cameras'] >= final_stats['total_cameras'] * 0.8:
            print(f"   ✅ BOA ESCALABILIDADE: {final_stats['active_cameras']}/{final_stats['total_cameras']} câmeras ativas")
        else:
            print(f"   ⚠️  ESCALABILIDADE LIMITADA: {final_stats['active_cameras']}/{final_stats['total_cameras']} câmeras ativas")
        
        # Avaliar performance
        if final_stats['avg_fps_per_camera'] >= 0.5:
            print(f"   ✅ BOA PERFORMANCE: {final_stats['avg_fps_per_camera']:.2f} FPS por câmera")
        else:
            print(f"   ⚠️  PERFORMANCE BAIXA: {final_stats['avg_fps_per_camera']:.2f} FPS por câmera")
        
        # Avaliar uso de recursos
        if final_sys_stats['cpu_percent'] < 80:
            print(f"   ✅ CPU ADEQUADA: {final_sys_stats['cpu_percent']:.1f}%")
        else:
            print(f"   ⚠️  CPU ALTA: {final_sys_stats['cpu_percent']:.1f}%")
        
        if final_sys_stats['memory_mb'] < 2000:
            print(f"   ✅ MEMÓRIA ADEQUADA: {final_sys_stats['memory_mb']:.1f} MB")
        else:
            print(f"   ⚠️  MEMÓRIA ALTA: {final_sys_stats['memory_mb']:.1f} MB")
        
        # Parar todas as câmeras
        print(f"\n[7] Parando todas as câmeras...")
        stopped_count = 0
        
        for camera_id in range(1, num_cameras + 1):
            if processor.stop_camera(camera_id):
                stopped_count += 1
                if camera_id % 10 == 0:
                    print(f"   Paradas {camera_id} câmeras...")
        
        print(f"[SUCESSO] {stopped_count} câmeras paradas")
        
        # Shutdown
        print("\n[8] Finalizando processador...")
        processor.shutdown()
        
        print("\n[SUCESSO] Teste de múltiplos streams concluído!")
        return True
        
    except Exception as e:
        print(f"\n[ERRO] Exceção durante teste: {e}")
        import traceback
        traceback.print_exc()
        return False

if __name__ == "__main__":
    success = main()
    exit(0 if success else 1) 